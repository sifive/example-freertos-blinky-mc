/* Copyright 2019 SiFive, Inc */
/* SPDX-License-Identifier: Apache-2.0 */



/******************************************************************************
 *
 * main() creates one queue, and two tasks.  It then starts the
 * scheduler.
 *
 * The Queue Send Task:
 * The queue send task is implemented by the prvQueueSendTask() function in
 * this file.  prvQueueSendTask() sits in a loop that causes it to repeatedly
 * block for 1000 milliseconds, before sending the value 100 to the queue that
 * was created within main().  Once the value is sent, the task loops
 * back around to block for another 1000 milliseconds...and so on.
 *
 * The Queue Receive Task:
 * The queue receive task is implemented by the prvQueueReceiveTask() function
 * in this file.  prvQueueReceiveTask() sits in a loop where it repeatedly
 * blocks on attempts to read data from the queue that was created within
 * blinky().  When data is received, the task checks the value of the
 * data, and if the value equals the expected 100, writes 'Blink' to the UART
 * (the UART is used in place of the LED to allow easy execution in QEMU).  The
 * 'block time' parameter passed to the queue receive function specifies that
 * the task should be held in the Blocked state indefinitely to wait for data to
 * be available on the queue.  The queue receive task will only leave the
 * Blocked state when the queue send task writes to the queue.  As the queue
 * send task writes to the queue every 1000 milliseconds, the queue receive
 * task leaves the Blocked state every 1000 milliseconds, and therefore toggles
 * the LED every 1 second.
 */

/* Standard includes. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Kernel includes. */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

/* Freedom metal includes. */
#include <metal/machine.h>
#include <metal/machine/platform.h>
#include <metal/lock.h>
#include <metal/uart.h>
#include <metal/interrupt.h>
#include <metal/clock.h>
#include <metal/led.h>

#if (__METAL_DT_MAX_HARTS <= 1)
#error "This example run only on multicore - please use the example-freertos-blinky"
#endif

extern struct metal_led *led0_red, *led0_green, *led0_blue;

/* Priorities used by the tasks. */
#define mainQUEUE_RECEIVE_TASK_PRIORITY		( tskIDLE_PRIORITY + 2 )
#define	mainQUEUE_SEND_TASK_PRIORITY		( tskIDLE_PRIORITY + 1 )

/* The rate at which data is sent to the queue.  The 200ms value is converted
to ticks using the pdMS_TO_TICKS() macro. */
#define mainQUEUE_SEND_FREQUENCY_MS			pdMS_TO_TICKS( 1000 )

/* The maximum number items the queue can hold.  The priority of the receiving
task is above the priority of the sending task, so the receiving task will
preempt the sending task and remove the queue items each time the sending task
writes to the queue.  Therefore the queue will never have more than one item in
it at any time, and even with a queue length of 1, the sending task will never
find the queue full. */
#define mainQUEUE_LENGTH					( 1 )

/*-----------------------------------------------------------*/
/*
 * Functions:
 * 		- prvSetupHardware: Setupe Hardware according CPU and Board.
 */
static void prvSetupHardware( void );

/*
 * The tasks as described in the comments at the top of this file.
 */
static void prvQueueReceiveTask( void *pvParameters );
static void prvQueueSendTask( void *pvParameters );

/*-----------------------------------------------------------*/

/* The queue used by both tasks. */
static QueueHandle_t xQueue = NULL;

struct metal_cpu *cpu0;
struct metal_interrupt *cpu_intr, *tmr_intr;
struct metal_led *led0_red, *led0_green, *led0_blue;

#define LED_ERROR ((led0_red == NULL) || (led0_green == NULL) || (led0_blue == NULL))

METAL_LOCK_DECLARE(my_lock);

int main(void);
int other_main();

/* This flag tells the secondary harts when to start to make sure
 * that they wait until the lock is initialized */
volatile int _start_other = 0;

/* This is a count of the number of harts who have executed their main function */
volatile int checkin_count = 0;

/* The secondary_main() function can be redefined to start execution
 * on secondary harts. We redefine it here to cause harts with
 * hartid != 0 to execute other_main() */
int secondary_main(void) {
	const char * const pcFailMessage = "Failed to initialize my_lock\r\n";
	int hartid = metal_cpu_get_current_hartid();

	if(hartid == 0) {
		int rc = metal_lock_init(&my_lock);
		if(rc != 0) {
			write( STDOUT_FILENO, pcFailMessage, strlen( pcFailMessage ) );
			for( ;; );
		}

		/* Ensure that the lock is initialized before any readers of
		 * _start_other */
		__asm__ ("fence rw,w"); /* Release semantics */

		_start_other = 1;

		return main();
	} else {
		return other_main(hartid);
	}
}

int other_main(int hartid) {
	const char * const pcMessage = "Other Hart Init\r\n";

	while(!_start_other) ;	

	metal_lock_take(&my_lock);
	write( STDOUT_FILENO, pcMessage, strlen( pcMessage ) );
	checkin_count += 1;

	metal_lock_give(&my_lock);

	while(1) {
		__asm__("wfi");
	}
}

/*-----------------------------------------------------------*/
int main( void )
{
	const char * const pcMessage = "FreeRTOS Demo Multicore start after other core init OK\r\n";

	prvSetupHardware();
	write( STDOUT_FILENO, pcMessage, strlen( pcMessage ) );

	/* Create the queue. */
	xQueue = xQueueCreate( mainQUEUE_LENGTH, sizeof( uint32_t ) );

	if( xQueue != NULL )
	{
		/* Start the two tasks as described in the comments at the top of this
		file. */
		xTaskCreate( prvQueueReceiveTask,				/* The function that implements the task. */
					"Rx", 								/* The text name assigned to the task - for debug only as it is not used by the kernel. */
					configMINIMAL_STACK_SIZE, 			/* The size of the stack to allocate to the task. */
					NULL, 								/* The parameter passed to the task - not used in this case. */
					mainQUEUE_RECEIVE_TASK_PRIORITY, 	/* The priority assigned to the task. */
					NULL );								/* The task handle is not required, so NULL is passed. */

		xTaskCreate( prvQueueSendTask, "TX", configMINIMAL_STACK_SIZE, NULL, mainQUEUE_SEND_TASK_PRIORITY, NULL );

		/* Start the tasks and timer running. */
		vTaskStartScheduler();
	}

	/* If all is well, the scheduler will now be running, and the following
	line will never be reached.  If the following line does execute, then
	there was insufficient FreeRTOS heap memory available for the Idle and/or
	timer tasks to be created.  See the memory management section on the
	FreeRTOS web site for more details on the FreeRTOS heap
	http://www.freertos.org/a00111.html. */
	for( ;; );
}
/*-----------------------------------------------------------*/

static void prvQueueSendTask( void *pvParameters )
{
	TickType_t xNextWakeTime;
	const unsigned long ulValueToSend = 100UL;
	BaseType_t xReturned;

	/* Remove compiler warning about unused parameter. */
	( void ) pvParameters;

	/* Initialise xNextWakeTime - this only needs to be done once. */
	xNextWakeTime = xTaskGetTickCount();

	for( ;; )
	{
		if ( led0_green != NULL ) 
		{
			/* Switch off the Green led */
        	metal_led_on(led0_green);
		}

		/* Place this task in the blocked state until it is time to run again. */
		vTaskDelayUntil( &xNextWakeTime, mainQUEUE_SEND_FREQUENCY_MS );

		/* Send to the queue - causing the queue receive task to unblock and
		toggle the LED.  0 is used as the block time so the sending operation
		will not block - it shouldn't need to block as the queue should always
		be empty at this point in the code. */
		xReturned = xQueueSend( xQueue, &ulValueToSend, 0U );
		configASSERT( xReturned == pdPASS );
	}
}
/*-----------------------------------------------------------*/

static void prvQueueReceiveTask( void *pvParameters )
{
	unsigned long ulReceivedValue;
	const unsigned long ulExpectedValue = 100UL;
	const char * const pcPassMessage = "Blink\r\n";
	const char * const pcFailMessage = "Unexpected value received\r\n";

	/* Remove compiler warning about unused parameter. */
	( void ) pvParameters;

	for( ;; )
	{
		/* Wait until something arrives in the queue - this task will block
		indefinitely provided INCLUDE_vTaskSuspend is set to 1 in
		FreeRTOSConfig.h. */
		xQueueReceive( xQueue, &ulReceivedValue, portMAX_DELAY );

		/*  To get here something must have been received from the queue, but
		is it the expected value?  If it is, toggle the LED. */
		if( ulReceivedValue == ulExpectedValue )
		{
			write( STDOUT_FILENO, pcPassMessage, strlen( pcPassMessage ) );
			ulReceivedValue = 0U;

			if ( led0_green != NULL ) 
			{
				/* Switch on the Green led */
				metal_led_off(led0_green);
			}
		}
		else
		{
			write( STDOUT_FILENO, pcFailMessage, strlen( pcFailMessage ) );
		}
	}
}
/*-----------------------------------------------------------*/

static void prvSetupHardware( void )
{
	const char * const pcErrorMsg = "No External controller\n";
	const char * const pcWarningMsg = "At least one of LEDs is null.\n";
	struct metal_cpu *cpu;
	struct metal_interrupt *cpu_intr;

	int num_harts = metal_cpu_get_num_harts();

	metal_lock_take(&my_lock);

	checkin_count += 1;

	metal_lock_give(&my_lock);

	while(checkin_count != num_harts) ;

	cpu = metal_cpu_get(metal_cpu_get_current_hartid());
	if (cpu == NULL) {
			return;
	}

	cpu_intr = metal_cpu_interrupt_controller(cpu);
	if (cpu_intr == NULL) {
			return;
	}
	metal_interrupt_init(cpu_intr);

	if (metal_interrupt_enable(cpu_intr, 0) == -1) {
			return;
	}

#ifdef METAL_RISCV_PLIC0
	{
		struct metal_interrupt *plic;

		// Check we this target has a plic. If not gracefull exit
		plic = metal_interrupt_get_controller(METAL_PLIC_CONTROLLER, 0);
		if (plic == NULL) {
			write( STDOUT_FILENO, pcErrorMsg, strlen( pcErrorMsg ) );

			for( ;; );
		} else {
			// disable all external interrupts
			*((volatile unsigned int *)(METAL_RISCV_PLIC0_C000000_BASE_ADDRESS + METAL_RISCV_PLIC0_ENABLE_BASE)) = 0;
			*((volatile unsigned int *)(METAL_RISCV_PLIC0_C000000_BASE_ADDRESS + METAL_RISCV_PLIC0_ENABLE_BASE+4)) = 0;
		}
	}
#endif

#ifdef METAL_SIFIVE_CLIC0
	{
	    struct metal_interrupt *clic;

		// Check we this target has a plic. If not gracefull exit
		clic = metal_interrupt_get_controller(METAL_CLIC_CONTROLLER, 0);
		if (clic == NULL) {
			write( STDOUT_FILENO, pcErrorMsg, strlen( pcErrorMsg ) );

			for( ;; );
		} else {
			// disable all external interrupts
			*((volatile unsigned int *)(METAL_SIFIVE_CLIC0_2000000_BASE_ADDRESS + METAL_SIFIVE_CLIC0_CLICINTIE_BASE)) = 0;
			*((volatile unsigned int *)(METAL_SIFIVE_CLIC0_2000000_BASE_ADDRESS + METAL_SIFIVE_CLIC0_CLICINTIE_BASE+4)) = 0;
		}
	}
#endif



    // This demo will toggle LEDs colors so we define them here
    led0_red = metal_led_get_rgb("LD0", "red");
    led0_green = metal_led_get_rgb("LD0", "green");
    led0_blue = metal_led_get_rgb("LD0", "blue");
    if ((led0_red == NULL) || (led0_green == NULL) || (led0_blue == NULL)) {
        write( STDOUT_FILENO, pcWarningMsg, strlen( pcWarningMsg ) );
    } else {
	    // Enable each LED
	    metal_led_enable(led0_red);
	    metal_led_enable(led0_green);
	    metal_led_enable(led0_blue);

	    // All Off
	    metal_led_on(led0_red);
	    metal_led_on(led0_green);
	    metal_led_on(led0_blue);
	}
}
/*-----------------------------------------------------------*/


void vApplicationMallocFailedHook( void )
{
	/* vApplicationMallocFailedHook() will only be called if
	configUSE_MALLOC_FAILED_HOOK is set to 1 in FreeRTOSConfig.h.  It is a hook
	function that will get called if a call to pvPortMalloc() fails.
	pvPortMalloc() is called internally by the kernel whenever a task, queue,
	timer or semaphore is created.  It is also called by various parts of the
	demo application.  If heap_1.c or heap_2.c are used, then the size of the
	heap available to pvPortMalloc() is defined by configTOTAL_HEAP_SIZE in
	FreeRTOSConfig.h, and the xPortGetFreeHeapSize() API function can be used
	to query the size of free heap space that remains (although it does not
	provide information on how the remaining heap might be fragmented). */
	taskDISABLE_INTERRUPTS();

   if ( led0_red != NULL )
   {
		// Red light on
		metal_led_off(led0_red);
	}

	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationIdleHook( void )
{
	/* vApplicationIdleHook() will only be called if configUSE_IDLE_HOOK is set
	to 1 in FreeRTOSConfig.h.  It will be called on each iteration of the idle
	task.  It is essential that code added to this hook function never attempts
	to block in any way (for example, call xQueueReceive() with a block time
	specified, or call vTaskDelay()).  If the application makes use of the
	vTaskDelete() API function (as this demo application does) then it is also
	important that vApplicationIdleHook() is permitted to return to its calling
	function, because it is the responsibility of the idle task to clean up
	memory allocated by the kernel to any task that has since been deleted. */
}
/*-----------------------------------------------------------*/

void vApplicationStackOverflowHook( TaskHandle_t pxTask, char *pcTaskName )
{
	( void ) pcTaskName;
	( void ) pxTask;

	/* Run time stack overflow checking is performed if
	configCHECK_FOR_STACK_OVERFLOW is defined to 1 or 2.  This hook
	function is called if a stack overflow is detected. */
	taskDISABLE_INTERRUPTS();

	write( STDOUT_FILENO, "ERROR Stack overflow on func: ", 30 );
	write( STDOUT_FILENO, pcTaskName, strlen( pcTaskName ) );


   if ( led0_red != NULL )
   {
		// Red light on
		metal_led_off(led0_red);
	}

	for( ;; );
}
/*-----------------------------------------------------------*/

void vApplicationTickHook( void )
{
	/* The tests in the full demo expect some interaction with interrupts. */
}
/*-----------------------------------------------------------*/

void vAssertCalled( void )
{
volatile uint32_t ulSetTo1ToExitFunction = 0;

	taskDISABLE_INTERRUPTS();

   if ( led0_red != NULL )
   {
		// Red light on
		metal_led_off(led0_red);
	}

	while( ulSetTo1ToExitFunction != 1 )
	{
		__asm volatile( "NOP" );
	}
}
