/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>

#include <platform.h>

#include "system.h"
#include "io.h"
#include "io_impl.h"
#include "rcc.h"

#include "bus_i2c.h"

#ifndef SOFT_I2C

#if defined(USE_I2C_PULLUP)
#define IOCFG_I2C IO_CONFIG(GPIO_Mode_AF, GPIO_Speed_50MHz, GPIO_OType_OD, GPIO_PuPd_UP)
#else
#define IOCFG_I2C IO_CONFIG(GPIO_Mode_AF, GPIO_Speed_50MHz, GPIO_OType_OD, GPIO_PuPd_NOPULL)
#endif

#define I2C_HIGHSPEED_TIMING  0x00500E30  // 1000 Khz, 72Mhz Clock, Analog Filter Delay ON, Setup 40, Hold 4.
#define I2C_STANDARD_TIMING   0x00E0257A  // 400 Khz, 72Mhz Clock, Analog Filter Delay ON, Rise 100, Fall 10.

#define I2C_SHORT_TIMEOUT   ((uint32_t)0x1000)
#define I2C_LONG_TIMEOUT    ((uint32_t)(10 * I2C_SHORT_TIMEOUT))
#define I2C_GPIO_AF         GPIO_AF_4

#ifndef I2C1_SCL
#define I2C1_SCL PB6
#endif
#ifndef I2C1_SDA
#define I2C1_SDA PB7
#endif
#ifndef I2C2_SCL
#define I2C2_SCL PF4
#endif
#ifndef I2C2_SDA
#define I2C2_SDA PA10
#endif

static void i2cUnstick(IO_t scl, IO_t sda);

typedef enum {
    I2C_STATE_STOPPED = 0,
    I2C_STATE_STOPPING,
    I2C_STATE_STARTING,
    I2C_STATE_STARTING_WAIT,

    I2C_STATE_R_ADDR,
    I2C_STATE_R_ADDR_WAIT,
    I2C_STATE_R_REGISTER,
    I2C_STATE_R_REGISTER_WAIT,
    I2C_STATE_R_RESTARTING,
    I2C_STATE_R_TRANSFER,

    I2C_STATE_W_ADDR,
    I2C_STATE_W_ADDR_WAIT,
    I2C_STATE_W_REGISTER,
    I2C_STATE_W_REGISTER_WAIT,
    I2C_STATE_W_RESTARTING,
    I2C_STATE_W_TRANSFER,

    I2C_STATE_NACK,
    I2C_STATE_BUS_ERROR,
} i2cState_t;

typedef enum {
    I2C_TXN_READ,
    I2C_TXN_WRITE
} i2cTransferDirection_t;

typedef struct i2cBusState_s {
    I2CDevice       device;
    bool            initialized;
    i2cState_t      state;
    uint32_t        timeout;

    /* Active transfer */
    uint8_t                     addr;   // device address
    i2cTransferDirection_t      rw;     // direction
    uint8_t                     reg;    // register
    uint32_t                    len;    // buffer length
    uint8_t                    *buf;    // buffer
    bool                        txnOk;
} i2cBusState_t;

static volatile uint16_t i2cErrorCount = 0;

static i2cDevice_t i2cHardwareMap[] = {
    { .dev = I2C1, .scl = IO_TAG(I2C1_SCL), .sda = IO_TAG(I2C1_SDA), .rcc = RCC_APB1(I2C1), .overClock = I2C1_OVERCLOCK },
    { .dev = I2C2, .scl = IO_TAG(I2C2_SCL), .sda = IO_TAG(I2C2_SDA), .rcc = RCC_APB1(I2C2), .overClock = I2C2_OVERCLOCK }
};

static i2cBusState_t busState[] = { { 0 } };

static void i2cResetInterface(i2cBusState_t * i2cBusState)
{
    const i2cDevice_t * i2c = &(i2cHardwareMap[i2cBusState->device]);
    IO_t scl = IOGetByTag(i2c->scl);
    IO_t sda = IOGetByTag(i2c->sda);

    i2cErrorCount++;
    i2cUnstick(scl, sda);
    i2cInit(i2cBusState->device);
}

static void i2cStateMachine(i2cBusState_t * i2cBusState)
{
    I2C_TypeDef * I2Cx = i2cHardwareMap[i2cBusState->device].dev;

    switch (i2cBusState->state) {
        case I2C_STATE_BUS_ERROR:
            i2cResetInterface(i2cBusState);
            i2cBusState->state = I2C_STATE_STOPPED;
            break;

        case I2C_STATE_STOPPING:
            if ((i2cBusState->timeout--) == 0) {
                i2cBusState->state = I2C_STATE_BUS_ERROR;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_ISR_STOPF) != RESET) {
                I2C_ClearFlag(I2Cx, I2C_ICR_STOPCF);
                i2cBusState->state = I2C_STATE_STOPPED;
            }
            break;

        case I2C_STATE_STOPPED:
            // Stick here
            break;

        case I2C_STATE_STARTING:
            i2cBusState->timeout = I2C_LONG_TIMEOUT;
            i2cBusState->state = I2C_STATE_STARTING_WAIT;
            // Fallthrough

        case I2C_STATE_STARTING_WAIT:
            if ((i2cBusState->timeout--) == 0) {
                i2cBusState->state = I2C_STATE_BUS_ERROR;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_ISR_BUSY) == RESET) {
                if (i2cBusState->rw == I2C_TXN_READ) {
                    i2cBusState->state = I2C_STATE_R_ADDR;
                }
                else {
                    i2cBusState->state = I2C_STATE_W_ADDR;
                }
            }
            break;

        case I2C_STATE_R_ADDR:
            /* Configure slave address, nbytes, reload, end mode and start or stop generation */
            I2C_TransferHandling(I2Cx, i2cBusState->addr, 1, I2C_SoftEnd_Mode, I2C_Generate_Start_Write);
            i2cBusState->state = I2C_STATE_R_ADDR_WAIT;
            i2cBusState->timeout = I2C_LONG_TIMEOUT;
            // Fallthrough

        case I2C_STATE_R_ADDR_WAIT:
            if ((i2cBusState->timeout--) == 0) {
                i2cBusState->state = I2C_STATE_BUS_ERROR;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_ISR_TXIS) != RESET) {
                i2cBusState->state = I2C_STATE_R_REGISTER;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_FLAG_NACKF) != RESET) {
                i2cBusState->state = I2C_STATE_NACK;
            }
            break;

        case I2C_STATE_R_REGISTER:
            I2C_SendData(I2Cx, i2cBusState->reg);
            i2cBusState->state = I2C_STATE_R_REGISTER_WAIT;
            i2cBusState->timeout = I2C_LONG_TIMEOUT;
            // Fallthrough

        case I2C_STATE_R_REGISTER_WAIT:
            if ((i2cBusState->timeout--) == 0) {
                i2cBusState->state = I2C_STATE_BUS_ERROR;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_ISR_TC) != RESET) {
                if (i2cBusState->len == 0) {
                    I2C_TransferHandling(I2Cx, i2cBusState->addr, 0, I2C_AutoEnd_Mode, I2C_Generate_Stop);
                    i2cBusState->state = I2C_STATE_STOPPING;
                }
                else {
                    i2cBusState->state = I2C_STATE_R_RESTARTING;
                }
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_FLAG_NACKF) != RESET) {
                i2cBusState->state = I2C_STATE_NACK;
            }
            break;

        case I2C_STATE_R_RESTARTING:
            I2C_TransferHandling(I2Cx, i2cBusState->addr, i2cBusState->len, I2C_AutoEnd_Mode, I2C_Generate_Start_Read);
            i2cBusState->state = I2C_STATE_R_TRANSFER;
            i2cBusState->timeout = I2C_LONG_TIMEOUT;
            // Fallthrough

        case I2C_STATE_R_TRANSFER:
            if ((i2cBusState->timeout--) == 0) {
                i2cBusState->state = I2C_STATE_BUS_ERROR;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_ISR_RXNE) != RESET) {
                *i2cBusState->buf++ = I2C_ReceiveData(I2Cx);
                i2cBusState->len--;

                if (i2cBusState->len == 0) {
                    // This was the last successful byte
                    i2cBusState->txnOk = true;
                    i2cBusState->state = I2C_STATE_STOPPING;
                }

                i2cBusState->timeout = I2C_LONG_TIMEOUT;
            }
            break;

        case I2C_STATE_W_ADDR:
            /* Configure slave address, nbytes, reload, end mode and start or stop generation */
            I2C_TransferHandling(I2Cx, i2cBusState->addr, 1, I2C_SoftEnd_Mode, I2C_Generate_Start_Write);
            i2cBusState->state = I2C_STATE_W_ADDR_WAIT;
            i2cBusState->timeout = I2C_LONG_TIMEOUT;
            // Fallthrough

        case I2C_STATE_W_ADDR_WAIT:
            if ((i2cBusState->timeout--) == 0) {
                i2cBusState->state = I2C_STATE_BUS_ERROR;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_ISR_TXIS) != RESET) {
                i2cBusState->state = I2C_STATE_W_REGISTER;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_FLAG_NACKF) != RESET) {
                i2cBusState->state = I2C_STATE_NACK;
            }
            break;

        case I2C_STATE_W_REGISTER:
            I2C_SendData(I2Cx, i2cBusState->reg);
            i2cBusState->state = I2C_STATE_W_REGISTER_WAIT;
            i2cBusState->timeout = I2C_LONG_TIMEOUT;
            // Fallthrough

        case I2C_STATE_W_REGISTER_WAIT:
            if ((i2cBusState->timeout--) == 0) {
                i2cBusState->state = I2C_STATE_BUS_ERROR;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_ISR_TC) != RESET) {
                if (i2cBusState->len == 0) {
                    I2C_TransferHandling(I2Cx, i2cBusState->addr, 0, I2C_AutoEnd_Mode, I2C_Generate_Stop);
                    i2cBusState->state = I2C_STATE_STOPPING;
                }
                else {
                    i2cBusState->state = I2C_STATE_W_RESTARTING;
                }
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_FLAG_NACKF) != RESET) {
                i2cBusState->state = I2C_STATE_NACK;
            }
            break;

        case I2C_STATE_W_RESTARTING:
            I2C_TransferHandling(I2Cx, i2cBusState->addr, i2cBusState->len, I2C_AutoEnd_Mode, I2C_No_StartStop);
            i2cBusState->state = I2C_STATE_W_TRANSFER;
            i2cBusState->timeout = I2C_LONG_TIMEOUT;
            // Fallthrough

        case I2C_STATE_W_TRANSFER:
            if ((i2cBusState->timeout--) == 0) {
                i2cBusState->state = I2C_STATE_BUS_ERROR;
            }
            else if (I2C_GetFlagStatus(I2Cx, I2C_ISR_TXIS) != RESET) {
                I2C_SendData(I2Cx, *i2cBusState->buf++);
                i2cBusState->len--;

                if (i2cBusState->len == 0) {
                    // This was the last successful byte
                    i2cBusState->txnOk = true;
                    i2cBusState->state = I2C_STATE_STOPPING;
                }

                i2cBusState->timeout = I2C_LONG_TIMEOUT;
            }
            break;

        case I2C_STATE_NACK:
            I2C_TransferHandling(I2Cx, i2cBusState->addr, 0, I2C_AutoEnd_Mode, I2C_Generate_Stop);
            I2C_ClearFlag(I2Cx, I2C_FLAG_NACKF);
            i2cBusState->state = I2C_STATE_STOPPING;
            break;
    }
}

void i2cSetOverclock(uint8_t overClock)
{
    for (unsigned int i = 0; i < sizeof(i2cHardwareMap) / sizeof(i2cHardwareMap[0]); i++) {
        i2cHardwareMap[i].overClock = overClock;
    }
}

uint32_t i2cTimeoutUserCallback(void)
{
    i2cErrorCount++;
    return false;
}

void i2cInit(I2CDevice device)
{
    if (device == I2CINVALID)
        return;

    i2cDevice_t *i2c = &(i2cHardwareMap[device]);

    IO_t scl = IOGetByTag(i2c->scl);
    IO_t sda = IOGetByTag(i2c->sda);

    RCC_ClockCmd(i2c->rcc, ENABLE);
    RCC_I2CCLKConfig(i2c->dev == I2C2 ? RCC_I2C2CLK_SYSCLK : RCC_I2C1CLK_SYSCLK);

    IOInit(scl, OWNER_I2C, RESOURCE_I2C_SCL, RESOURCE_INDEX(device));
    IOConfigGPIOAF(scl, IOCFG_I2C, GPIO_AF_4);

    IOInit(sda, OWNER_I2C, RESOURCE_I2C_SDA, RESOURCE_INDEX(device));
    IOConfigGPIOAF(sda, IOCFG_I2C, GPIO_AF_4);

    I2C_InitTypeDef i2cInit = {
        .I2C_Mode = I2C_Mode_I2C,
        .I2C_AnalogFilter = I2C_AnalogFilter_Enable,
        .I2C_DigitalFilter = 0x00,
        .I2C_OwnAddress1 = 0x00,
        .I2C_Ack = I2C_Ack_Enable,
        .I2C_AcknowledgedAddress = I2C_AcknowledgedAddress_7bit,
        .I2C_Timing = (i2c->overClock ? I2C_HIGHSPEED_TIMING : I2C_STANDARD_TIMING)
    };

    I2C_Init(i2c->dev, &i2cInit);
    I2C_StretchClockCmd(i2c->dev, ENABLE);
    I2C_Cmd(i2c->dev, ENABLE);

    busState[device].device = device;
    busState[device].initialized = true;
    busState[device].state = I2C_STATE_STOPPED;
}

uint16_t i2cGetErrorCounter(void)
{
    return i2cErrorCount;
}

static void i2cWaitForCompletion(I2CDevice device)
{
    do {
        i2cStateMachine(&busState[device]);
    } while (busState[device].state != I2C_STATE_STOPPED);
}

bool i2cWrite(I2CDevice device, uint8_t addr, uint8_t reg, uint8_t data)
{
    static uint8_t writeBuf[1];

    // Set up write transaction
    writeBuf[0] = data;

    busState[device].addr = addr << 1;
    busState[device].reg = reg;
    busState[device].rw = I2C_TXN_WRITE;
    busState[device].len = 1;
    busState[device].buf = writeBuf;
    busState[device].txnOk = false;
    busState[device].state = I2C_STATE_STARTING;

    // Inject I2C_EVENT_START
    i2cWaitForCompletion(device);

    return busState[device].txnOk;
}

bool i2cRead(I2CDevice device, uint8_t addr, uint8_t reg, uint8_t len, uint8_t* buf)
{
    // Set up read transaction
    busState[device].addr = addr << 1;
    busState[device].reg = reg;
    busState[device].rw = I2C_TXN_READ;
    busState[device].len = len;
    busState[device].buf = buf;
    busState[device].txnOk = false;
    busState[device].state = I2C_STATE_STARTING;

    // Inject I2C_EVENT_START
    i2cWaitForCompletion(device);

    return busState[device].txnOk;
}

static void i2cUnstick(IO_t scl, IO_t sda)
{
    int i;

    IOHi(scl);
    IOHi(sda);

    IOConfigGPIO(scl, IOCFG_OUT_OD);
    IOConfigGPIO(sda, IOCFG_OUT_OD);

    // Analog Devices AN-686
    // We need 9 clock pulses + STOP condition
    for (i = 0; i < 9; i++) {
        // Wait for any clock stretching to finish
        int timeout = 100;
        while (!IORead(scl) && timeout) {
            delayMicroseconds(5);
            timeout--;
        }

        // Pull low
        IOLo(scl); // Set bus low
        delayMicroseconds(5);
        IOHi(scl); // Set bus high
        delayMicroseconds(5);
    }

    // Generate a stop condition in case there was none
    IOLo(scl);
    delayMicroseconds(5);
    IOLo(sda);
    delayMicroseconds(5);

    IOHi(scl); // Set bus scl high
    delayMicroseconds(5);
    IOHi(sda); // Set bus sda high
}

#endif
