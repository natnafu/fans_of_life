#include "conway.h"
#include "fan.h"
#include "controller.h"
#include "physical.h"
#include "project.h"
#include "rs485.h"
#include "stopwatch.h"

//#define IS_PERIPHERAL
#define IS_CONTROLLER

// Config error checking
#if (defined IS_PERIPHERAL && defined IS_CONTROLLER)
#error  Can not define both controller and peripheral   
#elif (defined IS_CONTROLLER && UART_RX_HW_ADDRESS1 != 8)
#error  incorrect controller UART hardware address
#elif (defined IS_PERIPHERAL && UART_RX_HW_ADDRESS1 >= 8)
#error  incorrect peripheral UART hardware address
#endif

// Cell configuration bits
#define CONFIG_PULSE_TIME   (1 << 0)    // enables/disables pulsing all fans on before a read operation
#define CONFIG_PWM_FANS     (1 << 1)    // enables/disables PWMing fans that are on

#define CONFIG_DEFAULT      0           // default cell config, no pulsing or pwm

int main(void) {

    CyGlobalIntEnable;          // Enable global interrupts.
    Timer_Start();              // Used for stopwatch timers
    UART_Start();               // Used for RS485 comms
    UART_ClearRxBuffer();


#if (defined IS_PERIPHERAL)
    uint8_t rx_buff_size = 0;   // Stores UART RX buffer size
    uint32_t ctrl_state = 0;    // commanded state from controller
    uint32_t old_state  = 0;    // previous state
    uint32_t curr_state = 0;    // current state

    uint32_t config = CONFIG_DEFAULT;
    uint32_t cell_pulse_time_ms = 0;
    uint32_t cell_pwm_time_ms = 0;

    uint32_t timer_comm = 0;    // RS485 communication timer 

    uint32_t validation[FANS_PER_CELL] = {0};    // holds the validation times for each fan

    gpiox_init();   // Init GPIO expander ICs
    curr_state = fan_set_state(0, TOUT_FAN_SET);     // Init fan states to 0
#elif (defined IS_CONTROLLER)
    CyDelay(1000);
    controller_write_all(0);
    
#define CHANGE_TIMER_MS     6000
    uint32_t timer_change = stopwatch_start();   // counts the time since human input
    //CyDelay(6000);
#endif

    for(;;)
    {
#ifdef IS_PERIPHERAL
        // Handle pulsing. Turns all fans on for cell_pulse_time_ms and then restores state.
        // This is attempting to quiet the whining sound of the fans when off and trying to spin.
        // Depending on length, may create an audible clicking sound or have no effect.
        // NOTE: this doesn't really work :(
        if (cell_pulse_time_ms) {
            fan_set_state(UINT32_MAX, 0);
            CyDelay(cell_pulse_time_ms);
            fan_set_state(curr_state, 0);
        }

        // Handle PWMing. If enabled, turn on fan for cell_pwm_time_ms and then turn off.
        // This lowers the fan speed for less force on fingers. Trade off is a small audible clicking sound.
        if (cell_pwm_time_ms) {
            fan_set_state(curr_state, 0);
            CyDelay(cell_pwm_time_ms);
            fan_set_state(0,0);
        }

        // Handle updating fan state
        old_state = curr_state;         // Save state
        curr_state = fan_get_state();   // Get new state (200ms blocking time)

        // Handle validation
        for (uint32_t i = 0; i < FANS_PER_CELL; i++) {
            if (validation[i]) {
                if (curr_state & (1 << i)) {
                    // fan still spinning, check if validation expired
                    if (stopwatch_elapsed_ms(validation[i]) >= TOUT_FAN_SET) {
                        // validation expired, reset
                        validation[i] = 0;
                    } else {
                        // still validating, don't recognize spindown as human input
                        curr_state &= ~(1 << i);
                    }
                } else {
                    // fan not spinning, reset validation
                    validation[i] = 0;
                }
            }
        }

        // Handle human input
        if (curr_state != old_state) {
            fan_set_state(curr_state, 0);  // don't validate since human input has no spindown
        }

        // Check for commands
        rx_buff_size = UART_GetRxBufferSize();
        if (rx_buff_size == UART_RX_BUFFER_SIZE) {
            uint8 rx_cmd = UART_ReadRxData();   // first byte is READ/WRITE
            if (rx_cmd == UART_READ) {
                rs485_tx(CONTROLLER_ADDRESS, UART_READ, curr_state);    // send back current state
            } else if (rx_cmd == UART_WRITE) {
                // next 4 bytes are new state
                ctrl_state  = ((uint32_t) UART_ReadRxData() << 24) |
                              ((uint32_t) UART_ReadRxData() << 16) |
                              ((uint32_t) UART_ReadRxData() <<  8) |
                              ((uint32_t) UART_ReadRxData() <<  0);
                rs485_tx(CONTROLLER_ADDRESS, UART_WRITE, curr_state);   // send back confirmation that cmd was received
                curr_state = fan_set_ctrl(curr_state, ctrl_state, validation);
            } else if (rx_cmd == UART_CONFIG) {
                uint8_t config_option =  UART_ReadRxData(); // get type of config
                if (config_option == CONFIG_PULSE_TIME) {
                    cell_pulse_time_ms = UART_ReadRxData();
                } else if (config_option == CONFIG_PWM_FANS) {
                    cell_pwm_time_ms = UART_ReadRxData();
                }
            }
            // Clear RX buffer after processing data
            UART_ClearRxBuffer();
            timer_comm = 0;
        } else if (rx_buff_size != 0) {
            // If buffer isn't full or empty, clear the buffer if a timeout occurs
            if (timer_comm == 0) {
                // Set timer if it hasn't been started yet
                timer_comm = stopwatch_start();
            } else {
                if (stopwatch_elapsed_ms(timer_comm) >= TOUT_RX_COMM) {
                    // Comm timeout: reset timer and clear RX buffer
                    timer_comm = 0;
                    UART_ClearRxBuffer();    
                }
            }
        }
#endif // peripheral

#ifdef IS_CONTROLLER 
        // Play Conway's Game of Life

        // Save last state
        memcpy(conway_last_frame, conway_curr_frame, sizeof(conway_curr_frame));
        // Read grid
        controller_read_grid(conway_curr_frame);
        // Check if it's change        
        if (conway_has_changed() >= 2) {
            timer_change = stopwatch_start();    
        }
        // Update only if timer has expired
        if (stopwatch_elapsed_ms(timer_change) >= CHANGE_TIMER_MS) {
            conway_update_frame();
            controller_write_grid(conway_curr_frame);
            timer_change = stopwatch_start();
        }
/*
        // Tests turning all fans on and off
        while(1) {
            controller_write_all(0);
            CyDelay(5000);
            controller_write_all(UINT32_MAX);
            CyDelay(5000);
        }
               
        // Tests conway wiht no human input
        memcpy(conway_curr_frame, conway_dead, sizeof(conway_curr_frame));
        controller_write_grid(conway_curr_frame);
        CyDelay(6000);
        while(1) {
            conway_update_frame();
            controller_write_grid(conway_curr_frame);
            CyDelay(6000);
        }

        // Tests writing cells
        uint32_t state_test;
        for (uint8_t cell = 0; cell < NUM_CELLS; cell++) {
        //for (uint8_t cell = 1; cell < 2; cell++) {
            state_test = 0;
            for (uint8_t fan = 0; fan < FANS_PER_CELL; fan++) {
                state_test |= (1 << fan);
                controller_write_cell(cell, state_test);
                CyDelay(500);   // min peripheral response time is ~200ms because of blocking fan_get_state()
                // NOTE: this delay only works for setting. To be robust to all commands, this delay should be
                // greater than or equal to the fan validation timeout. Possibly could have the peripheral respond when
                // either the state has been set or its validation has timed out. All robust solutions result in
                // the grid getting updated every ~5s (spindown time).
            }
        }
*/
#endif
    }
}
