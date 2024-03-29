#include <FreeRTOS.h>
#include <task.h>
#include <board.h>
#include <string.h>

#include "app.h"
#include "adc.h"
#include "uart.h"
#include "mpu.h"
#include "bluetooth.h"
#include "messages.h"


// DEBUG
// Esta constante es para aumentar el intervalo de las tareas, en caso de
// querer que se disparen mas lento para debugging.
#define DBG_PERIOD_MULTIPLIER    1


/// Memoria estatica de la aplicacion, para no ponerla en el stack.
uint8_t buffer_queue_mem[APP_DATA_BUF_SIZE * APP_DATA_BUF_NMBR];


/**
 * Tarea principal, espera que haya muestras del ADC y las envia por la UART
 * Bluetooth, luego espera la respuesta por Bluetooth o enciende el LED de
 * error.
 */
void vTaskApp( void *pParam );

/**
 * Tarea del ADC, simplemente toma una muestra por iteracion y la coloca en un
 * buffer.  En caso de que se cambie el periodo de muestreo aqui adentro se
 * cambia el delay entre casa iteracion.
 * Se comunica con vTaskApp a traves de un buffer_queue.
 */
void vTaskADC( void *pParam );

/**
 * Trea de recepcion Bluetooth.  Esta escuchando la UART Bluetooth en caso de
 * recibir algun mensaje, para simplificar las cosas aceptamos cualquier mensaje
 * como ACKNOWLEDGE de que todo esta bien y disparamos el semaforo que le
 * notifica a vTaskApp que los datos se enviaron correctamente.
 */
void vTaskBluetooth( void *pParam );

/**
 * Anti-rebote de las teclas.  En caso de que haya algun evento guarda la nueva
 * configuracion en la SD y luego indica con un semaforo que hay que recargar la
 * config.  Por ahora lo unico que hace es cambiar la frecuencia de muestreo.
 */
void vTaskConfig( void *pParam );

/**
 * Solo se dispara cuando hubo un error de recepcion Bluetooth y queda encendida
 * por APP_ERROR_ONTIME.  La tarea queda trabada indefinidamente mientras no se
 * de esa condicion.
 */
void vTaskError( void *pParam );

/**
 * Esta tarea lee los valores del MPU cada APP_ACCEL_TASK_PERIOD milisegundos
 * y los manda a una FIFO.  Despues la tarea principal de la aplicacion (la que
 * envia por Bluetooth) lee esto y lo usa para modificar las muestras del ADC
 * por enviar.
 */
void vTaskMPU( void *pParam );


void app_update( app_type* app )
{
    // Primero vemos si hay que actualizar los parametros del accelerometro.
    float new_accel[3];
    if (xQueueReceive(app->queue_mpu, new_accel, 0) == pdPASS)
    {
        app->accel[0] = new_accel[0];
        app->accel[1] = new_accel[1];
        app->accel[2] = new_accel[2];
    }

    // Pedimos un buffer lleno con muestras del ADC.
    // El timeout esta por si las dudas, si las cosas andan bien y no le paso
    // nada raro a la tarea del ADC siempre vamos a tener datos para procesar.
    const TickType_t timeout = pdMS_TO_TICKS(1000UL * DBG_PERIOD_MULTIPLIER);
    uint8_t* buf = buffer_queue_get_inuse(&app->data_queue, timeout);

    if (buf != NULL)
    {
        float mult = app->accel[0];
        //mult = 1.0;
        for (unsigned i = 0; i < APP_DATA_BUF_SIZE; ++i)
            bluetooth_write(buf[i] * mult);
        buffer_queue_return(&app->data_queue, buf);

        const TickType_t bluetooth_timeout = pdMS_TO_TICKS(APP_BLUETOOTH_TIMEOUT);
        if (xSemaphoreTake(app->semaphore_reply, bluetooth_timeout) != pdTRUE)
        {
            // Timeout
            xSemaphoreGive(app->semaphore_error);
        }
    }
    else
    {
        // ERROR
    }
}

void adc_update( app_type* app )
{
    uint8_t* buf = app->current_buffer;
    if (buf == NULL)
    {
        // Tenemos que pedir un buffer nuevo.  Puede que no haya ninguno
        // disponible si nadie los vacio todavia, en tal caso obtenemos el
        // proximo en uso y lo descartamos, seria como hacer una especia de
        // buffer circular.
        buf = buffer_queue_get_avail(&app->data_queue, 0);
        if (buf == NULL)
        {
            buf = buffer_queue_get_inuse(&app->data_queue, 0);
            if (buf != NULL)
            {
                buffer_queue_return(&app->data_queue, buf);
                buf = NULL;
            }
            else
            {
                // ERROR
            }
        }
        app->samples_in_buffer = 0;
        app->current_buffer = buf;
    }

    if (buf != NULL) // Solo leemos el ADC si tenemos un buffer disponible
    {
        buf[app->samples_in_buffer++] = adc_read(APP_ADC_CHANNEL);

        if (app->samples_in_buffer == APP_DATA_BUF_SIZE)
        {
            // Se lleno el buffer actual, enviarlo y marcarlo para pedir uno
            // nuevo en la proxima iteracion.
            buffer_queue_push(&app->data_queue, buf);
            app->current_buffer = NULL;
        }
    }
}

void bluetooth_update( app_type* app )
{
    uint8_t data;
    if (bluetooth_read(&data))
    {
        // Indicamos a vTaskApp que esta todo bien.
        xSemaphoreGive(app->semaphore_reply);
    }
}

void buttons_update( app_type* app )
{
    debouncer_update(&app->button_left );
    debouncer_update(&app->button_right);
    debouncer_update(&app->button_up   );
    debouncer_update(&app->button_down );
}

void config_update( app_type* app )
{
    int modify_sample_rate = 0;

    // Bajar la frecuencia de muestreo.
    if (debouncer_is_edge(&app->button_left))
    {
        if (debouncer_is_hi(&app->button_left))
        {
            Board_LED_Set(LED_3, 0);
            modify_sample_rate = -1;
        }
        else
        {
            Board_LED_Set(LED_3, 1);
        }
    }

    // Aumentar la frecuencia de muestreo.
    if (debouncer_is_edge(&app->button_right))
    {
        if (debouncer_is_hi(&app->button_right))
        {
            Board_LED_Set(LED_3, 0);
            modify_sample_rate = 1;
        }
        else
        {
            Board_LED_Set(LED_3, 1);
        }
    }

    if (modify_sample_rate != 0)
    {
        if (modify_sample_rate > 0 && app->config.sample_period < APP_ADC_MAX_RATE)
            app->config.sample_period++;
        if (modify_sample_rate < 0 && app->config.sample_period > APP_ADC_MIN_RATE)
            app->config.sample_period--;

        // Escribir la nueva config en la SD.
        if (app->config_sd_present)
        {
            if (config_write(APP_SD_CONFIG_FILENAME, &app->config) < 0)
                messages_print("ERROR: escribir el archivo de configuracion\n\r");
        }

        xSemaphoreGive(app->semaphore_config);
    }
}

void app_init( app_type* app )
{
    Board_Init();

    // Antes que nada inicializamos los mensajes de salida por UART, esto es
    // porque corren en su propia tarea y tienen una FIFO asociada.  Si
    // cualquier otra rutina usara esto en el arranque se romperia todo por no
    // estar creada la FIFO.
    messages_init( tskIDLE_PRIORITY+5 );
    
    // Unicializamos el modulo bluetooth antes de todo el resto porque se usa
    // por varias tareas en simultaneo.
    bluetooth_init();
    
    // Periodo de muestreo al maximo y el acelerometro en 0
    app->config.sample_period = 0;
    app->accel[0] = 0.0;
    app->accel[1] = 0.0;
    app->accel[2] = 0.0;

    // Inicializamos los semaforos y listas.
    app->semaphore_config = xSemaphoreCreateBinary();
    app->semaphore_error  = xSemaphoreCreateBinary();
    app->semaphore_reply  = xSemaphoreCreateBinary();
    app->queue_mpu        = xQueueCreate(1, sizeof(float[3]));

    // Inicializamos la lista de buffers.
    buffer_queue_init( &app->data_queue,
                       buffer_queue_mem,
                       APP_DATA_BUF_SIZE,
                       APP_DATA_BUF_NMBR );

    // Iniciamos todas las tareas, estan ordenadas por prioridad.
    xTaskCreate( vTaskADC,
                 (const char*) "Task ADC",
                 configMINIMAL_STACK_SIZE,
                 app,
                 tskIDLE_PRIORITY+1,
                 NULL );

    xTaskCreate( vTaskApp,
                 (const char*) "Task APP",
                 configMINIMAL_STACK_SIZE,
                 app,
                 tskIDLE_PRIORITY+2,
                 NULL );

    xTaskCreate( vTaskBluetooth,
                 (const char*) "Task Bluetooth",
                 configMINIMAL_STACK_SIZE,
                 app,
                 tskIDLE_PRIORITY+2,
                 NULL );

    xTaskCreate( vTaskConfig,
                 (const char*) "Task Config",
                 configMINIMAL_STACK_SIZE*2,
                 app,
                 tskIDLE_PRIORITY+3,
                 NULL );

    xTaskCreate( vTaskError,
                 (const char*) "Task Error",
                 configMINIMAL_STACK_SIZE,
                 app,
                 tskIDLE_PRIORITY+3,
                 NULL );

    xTaskCreate( vTaskMPU,
                 (const char*) "Task MPU",
                 configMINIMAL_STACK_SIZE,
                 app,
                 tskIDLE_PRIORITY+4,
                 NULL );
}


void vTaskApp( void *pParam )
{
    app_type* pApp = pParam;
    
    while (1)
    {
        app_update(pApp);  // Adentro espera por un buffer con datos.
    }
}

void vTaskADC( void *pParam )
{
    app_type* pApp = pParam;
    TickType_t xTaskDelay = pdMS_TO_TICKS((pApp->config.sample_period+1)*10 * DBG_PERIOD_MULTIPLIER);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    adc_init();
    pApp->current_buffer = NULL;
    
    while (1)
    {
        adc_update(pApp);

        if (xSemaphoreTake(pApp->semaphore_config, 0))
        {
            // Nueva configuracion
            xTaskDelay = pdMS_TO_TICKS((pApp->config.sample_period+1)*10 * DBG_PERIOD_MULTIPLIER);
        }

        vTaskDelayUntil(&xLastWakeTime, xTaskDelay);
    }
}

void vTaskBluetooth( void *pParam )
{
    app_type* pApp = pParam;
    const TickType_t xTaskDelay = pdMS_TO_TICKS(10UL * DBG_PERIOD_MULTIPLIER);
    TickType_t xLastWakeTime = xTaskGetTickCount();
    
    while (1)
    {
        bluetooth_update(pApp);

        vTaskDelayUntil(&xLastWakeTime, xTaskDelay);
    }
}

void vTaskConfig( void *pParam )
{
    app_type* pApp = pParam;
    const TickType_t xTaskDelay = pdMS_TO_TICKS(40UL * DBG_PERIOD_MULTIPLIER);
    TickType_t xLastWakeTime = xTaskGetTickCount();

    debouncer_init(&pApp->button_left,  2, APP_BUTTON_PIN_LEFT );
    debouncer_init(&pApp->button_right, 2, APP_BUTTON_PIN_RIGHT);
    debouncer_init(&pApp->button_up,    2, APP_BUTTON_PIN_UP   );
    debouncer_init(&pApp->button_down,  2, APP_BUTTON_PIN_DOWN );

    Board_LED_Set(LED_2, 1);
    pApp->config_sd_present = 1;
    if (config_init(APP_SD_CONFIG_FILENAME, &pApp->config) < 0)
    {
        messages_print("ERROR: FATFS/SD, usando configuracion por defecto.\n\r");
        pApp->config.sample_period = 0;
        pApp->config_sd_present = 0;
    }
    Board_LED_Set(LED_2, 0);

    messages_print("Sample period: ");
    char msg[2]; // Sabemos que el periodo nunca es >9 asi que entra en un char
    msg[0] = '0' + pApp->config.sample_period;
    msg[1] = '\0';
    messages_print(msg);
    messages_print("\n\r");
    
    while (1)
    {
        buttons_update(pApp);
        config_update(pApp);

        vTaskDelayUntil(&xLastWakeTime, xTaskDelay);
    }
}

void vTaskError( void *pParam )
{
    app_type* pApp = pParam;
    const TickType_t xTaskDelay = pdMS_TO_TICKS(APP_ERROR_ONTIME);
    
    while (1)
    {
        Board_LED_Set(LED_1, 0);
        xSemaphoreTake(pApp->semaphore_error, portMAX_DELAY);
        Board_LED_Set(LED_1, 1);
        vTaskDelay(xTaskDelay);
    }
}

void vTaskMPU( void *pParam )
{
    app_type* pApp = pParam;
    const TickType_t xTaskDelay = pdMS_TO_TICKS(APP_ACCEL_TASK_PERIOD);

    mpu_init();
    
    float accel[3];
    while (1)
    {
        mpu_get_accelerometer(accel);
        xQueueSendToBack(pApp->queue_mpu, accel, 0);
        vTaskDelay(xTaskDelay);
    }
}
