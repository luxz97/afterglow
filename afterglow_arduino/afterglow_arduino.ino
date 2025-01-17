/***********************************************************************
 *  afterglow:
 *      Copyright (c) 2018-2019 Christoph Schmid
 *
 ***********************************************************************
 *  This file is part of the afterglow pinball LED project:
 *  https://github.com/smyp/afterglow
 *
 *  afterglow is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  afterglow is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with afterglow.
 *  If not, see <http://www.gnu.org/licenses/>.
 ***********************************************************************/
 
//------------------------------------------------------------------------------
/* This code assumes following pin layout:
 *
 *  +----------+---------------+-----------+---------------+--------------+
 *  | Name     | Function      | Nano Pin# | Register, Bit | Mode         |
 *  +----------+---------------+-----------+---------------+--------------+
 *  | IN_DATA  | 74LS165 Q_H   | D2        | DDRD, 2       | Input        |
 *  | IN_CLK   | 74LS165 CLK   | D3        | DDRD, 3       | Output       |
 *  | IN_LOAD  | 74LS165 LD    | D4        | DDRD, 4       | Output       |
 *  | OUT_DATA | 74HC595 SER   | D5        | DDRD, 5       | Output       |
 *  | OUT_CLK  | 74LS595 SRCLK | D6        | DDRD, 6       | Output       |
 *  | OUT_LOAD | 74LS595 RCLK  | D7        | DDRD, 7       | Output       |
 *  | OE       | 74LS595 OE    | A1        | DDRC, 1       | Output       |
 *  | TEST1    | TESTMODE 1    | D8        | DDRB, 1       | Input Pullup |
 *  | TEST2    | TESTMODE 2    | D9        | DDRB, 2       | Input Pullup |
 *  | TEST3    | TESTMODE 3    | D10       | DDRB, 3       | Input Pullup |
 *  | TEST4    | TESTMODE 4    | D11       | DDRB, 4       | Input Pullup |
 *  | CM       | CURRENT MEAS  | A0        | DDRC, 0       | Input        |
 *  +----------+---------------+-----------+---------------+--------------+
*/

#include <EEPROM.h>
#include <avr/wdt.h>
#include <avr/boot.h>

//------------------------------------------------------------------------------
// Setup

// Afterglow version number
#define AFTERGLOW_VERSION 108

// Afterglow configuration version
#define AFTERGLOW_CFG_VERSION 1

// Afterglow board revision. Currently v1.3.
#define BOARD_REV 13

// turn debug output via serial on/off
#define DEBUG_SERIAL 0

// Number of consistent data samples required for matrix update
#define SINGLE_UPDATE_CONS 2

// original matrix update interval [us]
#define ORIG_INT (2000)

// local time interval, config A [us]
#define TTAG_INT_A (250)

// cycles per original interval, config A
#define ORIG_CYCLES_A (ORIG_INT / TTAG_INT_A)

// local time interval, config B [us]
#define TTAG_INT_B (500)

// cycles per original interval, config B
#define ORIG_CYCLES_B (ORIG_INT / TTAG_INT_B)

// number of columns in the lamp matrix
#define NUM_COL 8

// number of rows in the lamp matrix
#define NUM_ROW 8

// default glow duration [ms]
#define DEFAULT_GLOWDUR 140

// glow duration scaling in the configuration
#define GLOWDUR_CFG_SCALE 10

// default maximum lamp brightness 0-7
#define DEFAULT_BRIGHTNESS 7

// afterglow LED glow duration [ms]
#define AFTERGLOW_LED_DUR (2000)

// current supervision on pin A0
#define CURR_MEAS_PIN A0

// test mode setup
#define TEST_MODE_NUMMODES 7    // number of test modes
#define TEST_MODE_DUR 8         // test duration per mode [s]
#define TESTMODE_INT (500)      // test mode lamp switch interval [ms]
#define TESTMODE_CYCLES_A ((uint32_t)TESTMODE_INT * 1000UL / (uint32_t)TTAG_INT_A) // number of cycles per testmode interval, config A
#define TESTMODE_CYCLES_B ((uint32_t)TESTMODE_INT * 1000UL / (uint32_t)TTAG_INT_B) // number of cycles per testmode interval, config B


// enable lamp replay in test mode
//#define REPLAY_ENABLED

#ifdef REPLAY_ENABLED
// Replay time scale [us]
#define REPLAY_TTAG_SCALE 16000

// Replay record
typedef struct AG_LAMP_SWITCH_s
{
    uint16_t col : 3;    // lamp column
    uint16_t row : 3;    // lamp row
    uint16_t dttag : 10; // delta time tag [16ms] to the last event
} AG_LAMP_SWITCH_t;

// Replay logic
byte replay(void);

// Number of replay records
int numReplays(void);
#endif // REPLAY_ENABLED


//------------------------------------------------------------------------------
// serial port protocol definition

// write buffer size [bytes]
#define AG_CMD_WRITE_BUF 32

// command terminator character
#define AG_CMD_TERMINATOR ':'

// version poll command string
#define AG_CMD_VERSION_POLL "AGV"

// configuration poll command string
#define AG_CMD_CFG_POLL "AGCP"

// configuration save command string
#define AG_CMD_CFG_SAVE "AGCS"

// configuration reset to default command string
#define AG_CMD_CFG_DEFAULT "AGCD"

// data ready string
#define AG_CMD_CFG_DATA_READY "AGDR"

// acknowledge string
#define AG_CMD_ACK "AGCACK"

// NOT acknowledge string
#define AG_CMD_NACK "AGCNACK"


//------------------------------------------------------------------------------
// global variables

// Lamp matrix 'memory'
static uint16_t sMatrixState[NUM_COL][NUM_ROW];
    
// local time
static uint32_t sTtag = 0;

// interrupt runtime counters [cycles]
static uint16_t sLastIntTime = 0;
static uint16_t sMaxIntTime = 0;
static volatile uint16_t sOverflowCount = 0;

// remember the last column and row samples
static byte sLastColMask = 0;
static byte sLastRowMask = 0;

#if DEBUG_SERIAL
static byte sLastOutColMask = 0;
static byte sLastOutRowMask = 0;
static uint32_t sBadColCounter = 0;
static uint32_t sBadColOrderCounter = 0;
static byte sLastBadCol = 0;
static byte sLastGoodCol = 0;
static int sMaxCurr = 0;
static int sLastCurr = 0;
#endif

// afterglow configuration data definition
typedef struct AFTERGLOW_CFG_s
{
    uint16_t version;                         // afterglow version of the configuration
    uint16_t res;                             // reserved bytes
    uint8_t lampGlowDur[NUM_COL][NUM_ROW];    // Lamp matrix glow duration configuration [ms * GLOWDUR_CFG_SCALE]
    uint8_t lampBrightness[NUM_COL][NUM_ROW]; // Lamp matrix maximum brightness configuration (0-7)
    uint32_t crc;                             // data checksum
} AFTERGLOW_CFG_t;

// afterglow configuration
static AFTERGLOW_CFG_t sCfg;

// precalculated glow steps for each lamp
static uint16_t sGlowSteps[NUM_COL][NUM_ROW];

// precalculated maximum subcycle for lamp activation (brightness)
static byte sMaxSubcycle[NUM_COL][NUM_ROW];

// last state of PINB
static uint8_t sLastPINB = 0;


//------------------------------------------------------------------------------
void setup()
{
    noInterrupts(); // disable all interrupts

    // I/O pin setup
    // 74LS165 LOAD and CLK are output, DATA is input
    // 74HC595 LOAD, CLK and DATA are output
    DDRD = B11111001;
    // nano LED output on pin 13, testmode jumper on pin 10
    DDRB = B00100000;
    // activate the pullups for the testmode pins
    PORTB |= B00001111;
    // OE on A1, DBG on A2, current meas on A0
    DDRC = B00000110;
    // keep OE high
    PORTC |= B00000010;

    // Configure the ADC clock to 1MHz by setting the prescaler to 16.
    // This should allow for fast analog pin sampling without much loss of precision.
    // defines for setting and clearing register bits.
    _SFR_BYTE(ADCSRA) |= _BV(ADPS2);
    _SFR_BYTE(ADCSRA) &= ~_BV(ADPS1);
    _SFR_BYTE(ADCSRA) &= ~_BV(ADPS0);

    // initialize the data
    memset(sMatrixState, 0, sizeof(sMatrixState));

    // load the configuration from EEPROM
    int err;
    bool cfgLoaded = loadCfg(&err);
    if (cfgLoaded == false)
    {
        // set default configuration
        setDefaultCfg();

        // store the configuration to EEPROM
        saveCfgToEEPROM();
    }

    // Apply the configuration
    // This will prepare all values for the interrupt handlers.
    applyCfg();

    // enable serial output at 115200 baudrate
    Serial.begin(115200);
    Serial.print("afterglow v");
    Serial.print(AFTERGLOW_VERSION);
    Serial.println(" (c) 2018 morbid cornflakes");
    // check the extended fuse for brown out detection level
    uint8_t efuse = boot_lock_fuse_bits_get(GET_EXTENDED_FUSE_BITS);
    Serial.println("-----------------------------------------------");
    uint8_t bodBits = (efuse & 0x7);
    Serial.print("efuse BOD ");
    Serial.println((bodBits == 0x07) ? "OFF" : (bodBits == 0x04) ? "4.3V" : (bodBits == 0x05) ? "2.7V" : "1.8V");
#ifdef REPLAY_ENABLED
    Serial.print("Replay Table Size: ");
    Serial.println(numReplays());
#endif
#if DEBUG_SERIAL
    Serial.print("CFG from ");
    Serial.print(cfgLoaded ? "EEPROM" : "DEFAULT");
    if (err)
    {
        Serial.print(" err ");
        Serial.print(err);
    }
    Serial.println("");
#endif

    // setup the timers
    timerSetup();

    // enable all interrupts
    interrupts();

    // enable a strict 15ms watchdog
    wdt_enable(WDTO_15MS);

    sLastPINB = PINB;
}

//------------------------------------------------------------------------------
void timerSetup(void)
{
    // Use Timer1 to create an interrupt every TTAG_INT us.
    // This will be the heartbeat of our realtime task.
    TCCR1B = 0;
    // turn on CTC mode
    TCCR1B |= (1 << WGM12);
    // Set CS10 bit so timer runs at clock speed
    TCCR1B |= (1 << CS10);  
    // turn off other timer1 functions
    TCCR1A = 0;
    // set compare match register for TTAG_INT us increments
    // prescaler is at 1, so counting real clock cycles
    OCR1A = (PINB & B00000100) ?
        (TTAG_INT_A * 16) :  // [16MHz clock cycles]
        (TTAG_INT_B * 16);   // [16MHz clock cycles]
    // enable timer compare interrupt
    TIMSK1 |= (1 << OCIE1A);
}

//------------------------------------------------------------------------------
void start()
{
    // enable the timer compare interrupt
    TIMSK1 |= (1 << OCIE1A);

    // enable a strict 15ms watchdog
    wdt_enable(WDTO_15MS);
}

//------------------------------------------------------------------------------
void stop()
{
    // disable the watchdog
    wdt_disable();

    // disable the timer compare interrupt
    TIMSK1 &= ~(1 << OCIE1A);

    // pull OE high to disable all outputs
    PORTC |= B00000010;
}

//------------------------------------------------------------------------------
// Timer1 overflow handler
// 
ISR(TIMER1_OVF_vect)
{
    sOverflowCount++;
}

//------------------------------------------------------------------------------
// Timer1 interrupt handler
// This is the realtime task heartbeat. All the magic happens here.
ISR(TIMER1_COMPA_vect)
{   
    // time is running
    uint16_t startCnt = TCNT1;
    sTtag++;

    // kick the dog
    wdt_reset();

    // Drive the lamp matrix
    // This is done before updating the matrix to avoid having an irregular update
    // frequency due to varying update calculation times.
    if ((PINB & B00001000) == 0)
    {
        // pass-through mode
        driveLampMatrixPassThrough();
    }
    else
    {
        // afterglow mode
        driveLampMatrix();
    }

#if (BOARD_REV >= 13)
    // Measure the current flowing through the current measurement resistor
    // (R26 on AG v1.3).
    int cm = analogRead(CURR_MEAS_PIN);
#if DEBUG_SERIAL
    sLastCurr = cm;
    if (sLastCurr > sMaxCurr)
    {
        sMaxCurr = sLastCurr;
    }
#endif
#endif

    // 74HC165 16bit sampling
    uint16_t inData = sampleInput();
    bool validInput = true;

    // testmode input simulation (jumper J1 active)
    if ((PINB & B00000001) == 0)
    {
        // test mode
        inData = testModeInput();
    }

    byte inColMask = (inData >> 8); // LSB is col 0, MSB is col 7
    byte inRowMask = ~(byte)inData; // high means OFF, LSB is row 0, MSB is row 7

    // evaluate the column reading
    // only one bit should be set as only one column can be active at a time
    uint32_t inCol = NUM_COL;
    switch (inColMask)
    {
        case 0x01: inCol = 0; break;
        case 0x02: inCol = 1; break;
        case 0x04: inCol = 2; break;
        case 0x08: inCol = 3; break;
        case 0x10: inCol = 4; break;
        case 0x20: inCol = 5; break;
        case 0x40: inCol = 6; break;
        case 0x80: inCol = 7; break;
        default:
        {
            // This may happen if the sample is taken in between column transition.
            // Depending on the pinball ROM version the duration of this transition varies.
            // On a Whitewater with Home ROM LH6 (contains anti ghosting updates) this
            // gap was measured to be around 30us long.
            // Machines with anti-ghosting firmware will show a gap with no column enabled
            // for a while during the transition while older firmwares might have two
            // columns enabled at the same time due to slow transistor deactivation. Both
            // cases are caught here.
            // See also https://emmytech.com/arcade/led_ghost_busting/index.html for details.
#if DEBUG_SERIAL
            sBadColCounter++;
            sLastBadCol = inColMask;
#endif
            validInput = false;
        }
        break;
    }

    // The matrix is updated only once per original column cycle. The code
    // waits for a number of consecutive consistent information before updating the matrix.
    validInput &= updateValid(inColMask, inRowMask);

    // Update only with a valid input. If the input is invalid the current
    // matrix state is left unchanged.
    if (validInput)
    {
        // update the current column
        updateCol(inCol, inRowMask);

#if DEBUG_SERIAL
        if ((inCol != (sLastGoodCol+1)) && (inCol!=(sLastGoodCol-7)))
        {
            sBadColOrderCounter++;
        }
        sLastGoodCol = inCol;
#endif
    }

    // remember the last column and row samples
    sLastColMask = inColMask;
    sLastRowMask = inRowMask;

    // how long did it take?
    sLastIntTime = (TCNT1 - startCnt);
    if ((sLastIntTime > sMaxIntTime) && (sLastIntTime < (1000 * 16)))
    {
        sMaxIntTime = sLastIntTime;
    }
}

//------------------------------------------------------------------------------
void loop()
{
    // The main loop is used for low priority serial communication only.
    // All the lamp matrix fun happens in the timer interrupt.

    // count the loops (used for debug output below)
    static uint32_t loopCounter = 0;
    loopCounter++;

    // check for serial data
    static String cmd = "";
    static bool complete = false;
    while (Serial.available() && (complete == false))
    {
        char character = Serial.read();
        if (character != AG_CMD_TERMINATOR)
        {
            // add the character and wait for the command terminator
            cmd.concat(character);
        }
        else
        {
            // command complete
            complete = true;
        }
    }

    // handle complete commands
    if (complete)
    {
        // version poll
        if (cmd == AG_CMD_VERSION_POLL)
        {
            // Output the version numbers
            Serial.print(AG_CMD_VERSION_POLL);
            Serial.print(" ");
            Serial.print(AFTERGLOW_VERSION);
            Serial.print(" ");
            Serial.println(AFTERGLOW_CFG_VERSION);
        }

        // configuration poll
        else if (cmd == AG_CMD_CFG_POLL)
        {
            // send the full confiuration
            sendCfg();
        }

        // configuration reset
        else if (cmd == AG_CMD_CFG_DEFAULT)
        {
            // reset the configuration to default
            defaultCfg();
        }

        // configuration write
        else if (cmd == AG_CMD_CFG_SAVE)
        {
            // stop the matrix updates
            stop();

            // receive a new configuration
            receiveCfg();

            // resume operation
            start();
        }

        cmd = "";
        complete = false;
    }

    // watch out for interval configuration changes
    if ((PINB & B00000100) != (sLastPINB & B00000100))
    {
        // reinitialize the timers
        noInterrupts();
        timerSetup();
        sTtag = 0;
        sLastPINB = PINB;
        interrupts();
#if DEBUG_SERIAL
        Serial.print("New TTAG_INT: ");
        Serial.println((PINB & B00000100) ? TTAG_INT_A : TTAG_INT_B);
#endif
    }

#if DEBUG_SERIAL
    if ((loopCounter % 10) == 0)
    {
        // print the maximum interrupt runtime
        if ((PINB & B00000001) == 0)
        {
            Serial.println("TESTMODE!");
        }
        if ((PINB & B0000100) == 0)
        {
            Serial.println("REPLAY!");
        }
        if ((PINB & B0001000) == 0)
        {
            Serial.println("PASS THROUGH!");
        }
        Serial.print("TTAG_INT ");
        Serial.println((PINB & B00000100) ? TTAG_INT_A : TTAG_INT_B);
        Serial.print("INT dt max ");
        Serial.print(sMaxIntTime / 16);
        Serial.print("us last ");
        Serial.print(sLastIntTime / 16);
        Serial.print("us ovfl ");
        Serial.println(sOverflowCount);
        Serial.print("Bad col: ");
        Serial.print(sBadColCounter);
        Serial.print(" col ");
        Serial.print(sLastBadCol);
        Serial.print(" ord ");
        Serial.print(sBadColOrderCounter);
        Serial.print(" last good: ");
        Serial.println(sLastGoodCol);
        Serial.print("CM ");
        Serial.print(sLastCurr);
        Serial.print(" max ");
        Serial.println(sMaxCurr);
        // data debugging
        debugInputs(sLastColMask, sLastRowMask);
        debugOutput(sLastOutColMask, sLastOutRowMask);
        // dump the full matrix
        for (uint32_t c=0; c<NUM_COL; c++)
        {
            Serial.print("C");
            Serial.print(c);
            Serial.print(" + ");
            for (uint32_t r=0; r<NUM_ROW; r++)
            {
                Serial.print(sMatrixState[c][r]);
                Serial.print(" ");
            }
            Serial.println("");
        }
    }
#endif

    // wait 500ms
    delay(500);
}

//------------------------------------------------------------------------------
inline void updateMx(uint16_t *pMx, bool on, uint16_t step)
{
    if (on)
    {
        // increase the stored brightness value
        if (*pMx < (65535 - step))
        {
            *pMx += step;
        }
        else
        {
            *pMx = 0xffff;
        }
    }
    else
    {
        // decrease the stored brightness value
        if (*pMx > step)
        {
            *pMx -= step;
        }
        else
        {
            *pMx = 0;
        }
    }
}

//------------------------------------------------------------------------------
void updateCol(uint32_t col, byte rowMask)
{
    // paranoia check
    if (col >= NUM_COL)
    {
        return;
    }
    
    // get a pointer to the matrix column
    uint16_t *pMx = &sMatrixState[col][0];
    const uint16_t *pkStep = &sGlowSteps[col][0];

    // update all row values
    for (uint32_t r=0; r<NUM_ROW; r++)
    {
        // update the matrix value
        updateMx(pMx, (rowMask & 0x01), *pkStep);

        // next row
        pMx++;
        pkStep++;
        rowMask >>= 1;
    }
}

//------------------------------------------------------------------------------
uint16_t sampleInput(void)
{
    // drive CLK and LOAD low
    PORTD &= B11100111;
    
    // wait some time
    uint16_t data = 0;
    data+= 17;
    data-= 3;
    
    // drive LOAD high to save pin states
    PORTD |= B00010000;
    
    // clock in all data
    for (byte i=0; i<16; i++)
    {
        data <<= 1;                        // make way for the new bit
        PORTD &= B11110111;                // CLK low
        data |= ((PIND & B00000100) >> 2); // read data bit
        PORTD |= B00001000;                // CLK high
    }
    return data;
}

//------------------------------------------------------------------------------
void driveLampMatrixPassThrough()
{
    static byte sLastPassThroughColMask = 0;
    static byte sLastPassThroughRowMask = 0;

    // only update when changed
    if ((sLastColMask != sLastPassThroughColMask) ||
        (sLastRowMask != sLastPassThroughRowMask))
    {
        // update the output
        dataOutput(sLastColMask, sLastRowMask);

        // remember the new state
        sLastPassThroughColMask = sLastColMask;
        sLastPassThroughRowMask = sLastRowMask;
    }
}

//------------------------------------------------------------------------------
void driveLampMatrix()
{   
    // turn off everything briefly to avoid ghosting
    // the scope says this takes ~20us at 16MHz
    dataOutput(0x00, 0x00);

    // check which column we're currently updating
    uint32_t outCol = (sTtag % NUM_COL);

    // The original cycle is divided into ORIG_CYCLES column sub cycles.
    // These cycles are used to do PWM in order to adjust the lamp brightness.
    //
    // Illustration with ORIG_CYCLES==4 and four brightness steps B1-B4 and off (B0):
    //
    // * Lamp on
    //                      2ms 2ms ...
    // Orig col            1   2   3   4   5   6   7   8   1   2   3   4   5   6
    // afterglow col       12345678123456781234567812345678123456781234567812345
    // col cycle           1       2       3       4       1       2       3
    //
    // Brightness 1        *                               *
    // Brightness 2        *       *                       *       *
    // Brightness 3        *       *       *               *       *       *
    // Brightness 4        *       *       *       *       *       *       *
    uint32_t colCycle = (PINB & B00000100) ?
        ((sTtag / NUM_COL) % ORIG_CYCLES_A) :
        ((sTtag / NUM_COL) % ORIG_CYCLES_B);

    // prepare the data
    // LSB is row/col 0, MSB is row/col 7
    byte colData = (1 << outCol);
    byte rowData = 0;
    uint16_t *pMx = &sMatrixState[outCol][0];
    byte *pMaxSubCycle = &sMaxSubcycle[outCol][0];
    for (uint32_t r=0; r<NUM_ROW; r++)
    {
        // make room for the next bit
        rowData >>= 1;
        
        // nothing to do if the matrix value is zero (off)
        if (*pMx)
        {
            uint16_t subCycle = (PINB & B00000100) ?
                (*pMx / (65536 / ORIG_CYCLES_A)) :
                (*pMx / (65536 / ORIG_CYCLES_B));

            // limit to the configured maximum brightness
            if (subCycle > *pMaxSubCycle)
            {
                subCycle = *pMaxSubCycle;
            }

            // Lamps are turned on when the value in the matrix is not zero
            // and when the value is high enough for the current sub cycle.
            if (subCycle >= colCycle)
            {
                rowData |= 0x80;
            }
        }
        pMx++;
        pMaxSubCycle++;
    }

    // output the data
    dataOutput(colData, rowData);
#if DEBUG_SERIAL
    sLastOutColMask = colData;
    sLastOutRowMask = rowData;
#endif
}

//------------------------------------------------------------------------------
void dataOutput(byte colData, byte rowData)
{
    // This writes the 16bit column and row data to the two 74595 shift registers
    
    // pull RCLK (OUT_LOAD) and CLK low to start sending data
    PORTD &= B00111111;

    // prepare the data
    uint16_t data = ((rowData << 8) | colData);
    
    // clock out all data
    for (uint16_t bitMask=0x8000; bitMask>0; bitMask>>=1)
    {
        PORTD &= B10111111; // CLK low
        if (data & bitMask)
        {
            PORTD |= B00100000; // set data bit
        }
        else
        {
            PORTD &= B11011111; // clear data bit
        }
        PORTD |= B01000000; // CLK high
    }

    PORTD &= B10111111; // CLK low

    // pull RCLK high to latch the data
    PORTD |= B10000000;

    // Enable by pulling OE low.
    // This is only done here to ensure that the LEDs are not turned on before
    // the columns are duty cycled.
    PORTC &= B11111101;
}

//------------------------------------------------------------------------------
uint16_t testModeInput(void)
{
    // simulate the original column cycle
    byte col = (PINB & B00000100) ?
        ((sTtag / ORIG_CYCLES_A) % NUM_COL) :
        ((sTtag / ORIG_CYCLES_B) % NUM_COL);
    byte colMask = (1 << col);

    // populate the row
    byte rowMask = 0;

#ifdef REPLAY_ENABLED
    // test switch 2 activates the replay mode
    if ((PINB & B00000010) == 0)
    {
        // replay from table
        rowMask = replay(col);
    }
#endif

    // Start simulation if test switch 2 (replay mode) is inactive
    if ((PINB & B00000010) != 0)
    {       
        // loop through all available modes
        uint8_t m = (PINB & B00000100) ?
            (sTtag / (TEST_MODE_DUR * 1000000UL / TTAG_INT_A)) :
            (sTtag / (TEST_MODE_DUR * 1000000UL / TTAG_INT_B));
        uint32_t tmp = (PINB & B00000100) ?
            (sTtag / TESTMODE_CYCLES_A) :
            (sTtag / TESTMODE_CYCLES_B);
        switch (m % TEST_MODE_NUMMODES)
        {
            case 0:
            // cycle all columns
            {
                uint8_t c = (tmp % NUM_COL);
                if (c == col)
                {
                    rowMask = 0xff;
                }
            }
            break;
            case 1:
            // cycle all rows
            {
                uint8_t r = (tmp % NUM_ROW);
                rowMask |= (1 << r);
            }
            break;
            case 2:
            // cycle all columns (inverted)
            {
                uint8_t c = (tmp % NUM_COL);
                if (c != col)
                {
                    rowMask = 0xff;
                }
            }
            break;
            case 3:
            // cycle all rows (inverted)
            {
                uint8_t r = (tmp % NUM_ROW);
                rowMask = ~(1 << r);
            }
            break;
            case 4:
            // blink all lamps
            {
                if (tmp % 2)
                {
                    rowMask = 0xff;
                }
            }
            break;
            case 5:
            // switch between even and odd lamps
            // turn on every other column
            {
                if (col % 2 == (tmp % 2))
                {
                    rowMask = B01010101;
                    if (tmp % 3)
                    {
                        rowMask <<= 1;
                    }
                }
            }
            break;
            case 6:
            // cycle through all lamps individually with 4x speed
            {
                uint8_t l = (uint8_t)((tmp * 4) % (NUM_COL * NUM_ROW));
                uint8_t c = (l / NUM_ROW);
                uint8_t r = (l % NUM_COL);
                if (c == col)
                {
                    rowMask = (1 << r);
                }
            }
            break;
            default:
            break;
        }
    }

    // invert the row mask as in the original input HIGH means off
    rowMask = ~rowMask;

    return ((colMask << 8) | rowMask);
}

//------------------------------------------------------------------------------
bool updateValid(byte inColMask, byte inRowMask)
{
    static byte sConsistentSamples = 0;
    static byte sLastUpdColMask = 0x00;
    bool valid = false;

    // check if the current column has not been handled already
    if (inColMask != sLastUpdColMask)
    {
        // reset the counter when the data changes
        if ((inColMask != sLastColMask) || (inRowMask != sLastRowMask))
        {
            sConsistentSamples = 0;
        }
        // count number of consecutive samples with consistent data
        else if (sConsistentSamples < 255)
        {
            sConsistentSamples++;
        }

        // The matrix is updated only once per original column cycle.
        // The code waits for a number of consecutive consistent information
        // before updating the matrix.
        // This also avoids ghosting issues, see
        // https://emmytech.com/arcade/led_ghost_busting/index.html for details.
        if (sConsistentSamples >= (SINGLE_UPDATE_CONS-1))
        {
            sLastUpdColMask = inColMask;
            valid = true;
        }
    }
    return valid;
}

//------------------------------------------------------------------------------
void applyCfg()
{
    // calculate the glow steps and maximum subcycles
    uint16_t *pGS = &sGlowSteps[0][0];
    uint8_t *pGlowDur = &sCfg.lampGlowDur[0][0];
    uint8_t *pBrightness = &sCfg.lampBrightness[0][0];
    byte *pMaxSubCycle = &sMaxSubcycle[0][0];
    for (byte c=0; c<NUM_COL; c++)
    {
        for (byte r=0; r<NUM_COL; r++)
        {
            // brightness step per lamp matrix update (assumes one update per original matrix step)
            uint32_t glowDur = (*pGlowDur * GLOWDUR_CFG_SCALE);
            *pGS++ = (glowDur > 0) ?
                ((uint16_t)(65535 / ((glowDur * 1000) / ORIG_INT)) * NUM_COL) : 0xffff;

            // translate maximum brightness into maximum lamp driving subcycle
            *pMaxSubCycle++ = (PINB & B00000100) ?
                (*pBrightness >> (8/ORIG_CYCLES_A-1)) :
                (*pBrightness >> (8/ORIG_CYCLES_B-1));

            // next
            pGlowDur++;
            pBrightness++;
        }
    }
}

//------------------------------------------------------------------------------
void setDefaultCfg()
{
    // initialize configuration to default values
    memset(&sCfg, 0, sizeof(sCfg));
    sCfg.version = AFTERGLOW_CFG_VERSION;
    uint8_t *pGlowDur = &sCfg.lampGlowDur[0][0];
    uint8_t *pBrightness = &sCfg.lampBrightness[0][0];
    for (byte c=0; c<NUM_COL; c++)
    {
        for (byte r=0; r<NUM_ROW; r++)
        {
            *pGlowDur++ = (DEFAULT_GLOWDUR / GLOWDUR_CFG_SCALE);
            *pBrightness++ = DEFAULT_BRIGHTNESS;
        }
    }

    // calculate the crc
    uint16_t cfgSize = sizeof(sCfg);
    sCfg.crc = calculateCRC32((uint8_t*)&sCfg, cfgSize-sizeof(sCfg.crc));
}

//------------------------------------------------------------------------------
void defaultCfg()
{
    // set the default configuration
    setDefaultCfg();

    // send the acknowledge
    Serial.print(AG_CMD_ACK);
}

//------------------------------------------------------------------------------
int loadCfg(int *pErr)
{
    bool valid = false;
    *pErr = 0;

    // load the configuration from the EEPROM
    uint16_t cfgSize = sizeof(sCfg);
    uint8_t *pCfg = (uint8_t*)&sCfg;
    for (uint16_t i=0; i<cfgSize; i++)
    {
        *pCfg++ = EEPROM.read(i);
    }

    // check the version
    if (sCfg.version == AFTERGLOW_CFG_VERSION)
    {
        // check the CRC of the data
        uint32_t crc = calculateCRC32((uint8_t*)&sCfg, cfgSize-sizeof(sCfg.crc));
        if (crc == sCfg.crc)
        {
            valid = true;
        }
        else
        {
            *pErr = 2;
        }
    }
    else
    {
        *pErr = 1;
    }

    return valid;
}

//------------------------------------------------------------------------------
uint32_t calculateCRC32(const uint8_t *data, uint16_t length)
{
    uint32_t crc = 0xffffffff;
    while (length--)
    {
        uint8_t c = *data++;
        for (uint32_t i = 0x80; i > 0; i >>= 1)
        {
            bool bit = crc & 0x80000000;
            if (c & i)
            {
                bit = !bit;
            }
            crc <<= 1;
            if (bit)
            {
                crc ^= 0x04c11db7;
            }
        }
    }
    return crc;
}

//------------------------------------------------------------------------------
void sendCfg()
{
    // send the whole configuration structure via serial port
    uint16_t cfgSize = sizeof(sCfg);
    const byte *pkCfg = (const byte*)&sCfg;
    Serial.write(pkCfg, cfgSize);
}

//------------------------------------------------------------------------------
void receiveCfg()
{
    // wait for the full configuration data
    bool res = false;
    AFTERGLOW_CFG_t cfg;
    uint8_t *pCfg = (uint8_t*)&cfg;
    uint16_t cfgSize = sizeof(cfg);
    uint16_t size = 0;

    // read all data
    while (size < cfgSize)
    {
        // send data ready signal and wait for data
        Serial.print(AG_CMD_CFG_DATA_READY);
        delay(200);

        // read data
        uint32_t readBytes = 0;
        while ((Serial.available()) && (readBytes < AG_CMD_WRITE_BUF) && (size < cfgSize))
        {
            *pCfg++ = Serial.read();
            readBytes++;
            size++;
        }
    }

    if (size == sizeof(cfg))
    {
        // check the crc
        uint32_t crc = calculateCRC32((uint8_t*)&cfg, size-sizeof(cfg.crc));
        if (crc == cfg.crc)
        {
             // set the new configuration and apply it
            memcpy(&sCfg, &cfg, size);
            applyCfg();

            // store the configuration to EEPROM
            saveCfgToEEPROM();

            res = true;
        }
#if DEBUG_SERIAL
        else
        {
            Serial.print("CRC FAIL ");
            Serial.print(crc);
            Serial.print(" ");
            Serial.println(cfg.crc);
        }
#endif
    }
#if DEBUG_SERIAL
    else
    {
            Serial.print("SIZE MISMATCH: ");
            Serial.println(size);
    }
#endif

    // send ACK/NACK
    Serial.print(res ? AG_CMD_ACK : AG_CMD_NACK);
}

//------------------------------------------------------------------------------
void saveCfgToEEPROM()
{
    const uint8_t *pkCfg = (const uint8_t*)&sCfg;
    for (uint16_t i=0; i<sizeof(sCfg); i++)
    {
        EEPROM.write(i, *pkCfg++);
    }
    Serial.print("EEPROM write ");
    Serial.println(sizeof(sCfg));
}

#if DEBUG_SERIAL
//------------------------------------------------------------------------------
void debugInputs(byte inColMask, byte inRowMask)
{
    // output the data
    char msg[64];
    sprintf(msg, "IN C 0x%02X R 0x%02X\n", inColMask, inRowMask);
    Serial.print(msg);
}

//------------------------------------------------------------------------------
void debugOutput(byte outColMask, byte outRowMask)
{
    // output the data
    char msg[64];
    sprintf(msg, "OUT C 0x%02X R 0x%02X\n", outColMask, outRowMask);
    Serial.print(msg);
}
#endif


#ifdef REPLAY_ENABLED
// Recording of the attract mode from a Creature of the Black Lagoon pinball.
// Recorded from a modified pinmame version.
const AG_LAMP_SWITCH_t kLampReplay[] PROGMEM =
{
{7, 7, 0},   // +0.000s 0
{7, 7, 25}, {0, 5, 4}, {1, 4, 0}, {2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, 
{5, 5, 0}, {0, 1, 2}, {1, 1, 0}, {1, 7, 0}, {6, 1, 0}, {7, 1, 0}, {0, 5, 4}, {0, 6, 0}, {1, 5, 0}, {2, 2, 0}, 
{2, 6, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, 
{0, 1, 3}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {4, 3, 0}, {5, 2, 0}, 
{6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {7, 4, 0}, {1, 7, 2}, {4, 7, 0}, {6, 1, 0}, {0, 4, 2}, {0, 6, 0}, {1, 5, 0},   // +7.067s 7072
{1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {5, 3, 0}, {5, 4, 0}, 
{5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 6, 0}, 
{2, 7, 0}, {5, 5, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 5, 2}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, 
{4, 7, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 4, 0}, {0, 0, 2}, {0, 4, 0}, {1, 0, 0}, {1, 4, 0}, {1, 6, 0}, 
{2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {4, 1, 0}, {7, 0, 0}, {7, 6, 0},   // +7.167s 7168
{0, 3, 2}, {1, 3, 0}, {2, 7, 0}, {4, 5, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, 
{4, 4, 0}, {5, 2, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 4, 0}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, 
{4, 1, 0}, {5, 1, 0}, {5, 7, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {7, 1, 0}, {0, 4, 2}, {3, 6, 0}, {4, 5, 0}, 
{4, 6, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 5, 0}, {0, 6, 2}, {1, 2, 0}, {1, 5, 0}, {1, 6, 0}, {2, 1, 0},   // +7.333s 7328
{2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {6, 6, 0}, {7, 2, 0}, {0, 1, 2}, 
{0, 2, 0}, {0, 7, 0}, {1, 1, 0}, {4, 7, 0}, {5, 1, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 5, 0}, {7, 1, 0}, 
{7, 6, 0}, {0, 4, 3}, {0, 5, 0}, {1, 4, 0}, {2, 3, 0}, {2, 5, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, 
{5, 4, 0}, {7, 3, 0}, {7, 5, 0}, {0, 3, 2}, {1, 2, 0}, {1, 3, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 2, 0}, 
{5, 6, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 2, 2}, {0, 6, 0}, {0, 7, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0},   // +7.467s 7472
{4, 7, 0}, {5, 0, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 6, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {1, 4, 0}, 
{1, 5, 0}, {2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {6, 0, 0}, {7, 0, 0}, 
{7, 3, 0}, {0, 3, 2}, {0, 4, 0}, {1, 3, 0}, {1, 7, 0}, {4, 5, 0}, {5, 0, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, 
{6, 7, 0}, {0, 6, 2}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, 
{4, 4, 0}, {5, 3, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0},   // +7.600s 7600
{3, 0, 0}, {5, 5, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {7, 4, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {1, 7, 0}, 
{3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 5, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, 
{1, 6, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {6, 2, 0}, 
{7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 6, 0}, {2, 7, 0}, {4, 7, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 1, 0}, 
{7, 4, 0}, {7, 6, 0}, {0, 5, 2}, {1, 3, 0}, {1, 4, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 3, 0}, {3, 6, 0},   // +7.733s 7728
{3, 7, 0}, {4, 6, 0}, {5, 2, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {2, 2, 0}, {2, 5, 0}, 
{3, 2, 0}, {4, 1, 0}, {5, 7, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {0, 4, 3}, {0, 6, 0}, {1, 6, 0}, {2, 0, 0}, 
{2, 4, 0}, {2, 7, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 6, 0}, 
{0, 0, 2}, {1, 0, 0}, {1, 3, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {6, 4, 0}, 
{7, 0, 0}, {7, 3, 0}, {0, 3, 2}, {0, 5, 0}, {3, 5, 0}, {4, 1, 0}, {4, 5, 0}, {5, 1, 0}, {5, 2, 0}, {5, 5, 0},   // +7.867s 7872
{5, 7, 0}, {6, 3, 0}, {0, 4, 2}, {1, 1, 0}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 5, 0}, 
{3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, 
{0, 7, 0}, {1, 0, 0}, {5, 6, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 2, 0}, 
{2, 6, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 1, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, 
{7, 5, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {4, 3, 0}, {6, 6, 0},   // +8.000s 8000
{7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 4, 0}, {0, 7, 0}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 0, 0}, {5, 4, 0}, 
{5, 6, 0}, {5, 7, 0}, {6, 5, 0}, {7, 6, 0}, {0, 6, 2}, {1, 3, 0}, {1, 5, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, 
{2, 4, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {5, 3, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, 
{1, 2, 0}, {1, 7, 0}, {4, 4, 0}, {5, 2, 0}, {5, 5, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 4, 2}, {0, 5, 0}, 
{1, 4, 0}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 0, 0}, {5, 4, 0},   // +8.133s 8128
{5, 7, 0}, {7, 6, 0}, {0, 0, 2}, {1, 0, 0}, {1, 3, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {5, 6, 0}, 
{6, 0, 0}, {7, 0, 0}, {7, 3, 0}, {0, 3, 3}, {0, 6, 0}, {1, 7, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, 
{5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {6, 7, 0}, {7, 4, 0}, {0, 5, 2}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, 
{2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {6, 1, 0}, {7, 1, 0}, {0, 0, 2}, 
{0, 1, 0}, {1, 0, 0}, {2, 7, 0}, {4, 6, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 0, 0}, {7, 0, 0}, {0, 4, 2},   // +8.300s 8304
{0, 6, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 3, 0}, 
{7, 4, 0}, {7, 5, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 1, 0}, {4, 1, 0}, 
{5, 5, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 5, 0}, {2, 7, 0}, {3, 7, 0}, {4, 6, 0}, 
{4, 7, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, {0, 4, 2}, {1, 3, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, 
{2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {7, 5, 0},   // +8.400s 8400
{0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {4, 1, 0}, {4, 4, 0}, {5, 1, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 2, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, {4, 7, 0}, 
{5, 2, 0}, {7, 0, 0}, {7, 6, 0}, {0, 0, 2}, {0, 7, 0}, {1, 0, 0}, {1, 3, 0}, {1, 4, 0}, {2, 3, 0}, {2, 5, 0}, 
{3, 3, 0}, {5, 7, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {0, 3, 2}, {0, 4, 0}, {0, 6, 0}, {3, 5, 0}, {4, 3, 0}, 
{4, 4, 0}, {4, 5, 0}, {5, 1, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {0, 1, 2}, {1, 1, 0}, {1, 5, 0}, {1, 6, 0},   // +8.567s 8560
{2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {6, 5, 0}, {7, 0, 0}, 
{7, 1, 0}, {0, 0, 3}, {0, 5, 0}, {0, 7, 0}, {1, 0, 0}, {4, 6, 0}, {5, 0, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, 
{6, 4, 0}, {7, 5, 0}, {0, 4, 2}, {1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {3, 5, 0}, 
{3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 4, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 7, 0}, {2, 1, 0}, 
{2, 4, 0}, {3, 1, 0}, {5, 6, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {3, 7, 0},   // +8.700s 8704
{4, 6, 0}, {4, 7, 0}, {5, 0, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, {7, 6, 0}, {0, 3, 2}, {1, 2, 0}, 
{1, 3, 0}, {1, 4, 0}, {2, 2, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 7, 0}, 
{7, 2, 0}, {7, 3, 0}, {0, 2, 2}, {0, 4, 0}, {1, 7, 0}, {4, 4, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 6, 0}, 
{7, 4, 0}, {0, 6, 2}, {1, 0, 0}, {1, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, 
{3, 4, 0}, {3, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 3, 0}, {7, 0, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 3, 0},   // +8.833s 8832
{2, 7, 0}, {3, 3, 0}, {5, 5, 0}, {6, 0, 0}, {6, 7, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {2, 1, 0}, 
{2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 4, 0}, 
{0, 1, 2}, {1, 0, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {6, 1, 0}, 
{7, 0, 0}, {7, 1, 0}, {0, 0, 2}, {0, 6, 0}, {2, 7, 0}, {3, 6, 0}, {4, 1, 0}, {4, 6, 0}, {5, 3, 0}, {5, 5, 0}, 
{5, 6, 0}, {6, 0, 0}, {7, 5, 0}, {0, 5, 2}, {1, 2, 0}, {1, 4, 0}, {1, 5, 0}, {2, 1, 0}, {2, 2, 0}, {2, 5, 0},   // +8.967s 8960
{2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {7, 2, 0}, {0, 1, 3}, {0, 2, 0}, 
{1, 1, 0}, {4, 7, 0}, {5, 1, 0}, {5, 7, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {0, 4, 2}, {0, 6, 0}, {1, 6, 0}, 
{2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 1, 0}, {4, 6, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, 
{7, 5, 0}, {7, 6, 0}, {0, 3, 2}, {1, 2, 0}, {1, 3, 0}, {1, 5, 0}, {2, 2, 0}, {2, 6, 0}, {3, 2, 0}, {6, 3, 0}, 
{7, 2, 0}, {7, 3, 0}, {0, 2, 2}, {0, 5, 0}, {0, 7, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 1, 0},   // +9.100s 9104
{5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 2, 0}, {0, 4, 2}, {1, 0, 0}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 3, 0}, 
{2, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {5, 4, 0}, {7, 0, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, 
{1, 3, 0}, {4, 5, 0}, {5, 0, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {0, 5, 2}, 
{0, 6, 0}, {0, 7, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, 
{4, 4, 0}, {5, 2, 0}, {0, 1, 2}, {1, 0, 0}, {1, 1, 0}, {1, 4, 0}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {5, 7, 0},   // +9.233s 9232
{6, 5, 0}, {7, 0, 0}, {7, 1, 0}, {0, 0, 2}, {0, 4, 0}, {1, 7, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 0, 0}, 
{5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {6, 4, 0}, {7, 5, 0}, {0, 6, 2}, {1, 2, 0}, {1, 5, 0}, {1, 6, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {6, 6, 0}, {7, 2, 0}, {0, 1, 2}, 
{0, 2, 0}, {1, 1, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 5, 0}, {7, 1, 0}, {7, 4, 0}, {0, 4, 2}, 
{0, 5, 0}, {1, 4, 0}, {1, 7, 0}, {2, 3, 0}, {2, 5, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {5, 4, 0},   // +9.367s 9360
{7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {0, 3, 3}, {1, 2, 0}, {1, 3, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 2, 0}, 
{5, 6, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 2, 2}, {0, 6, 0}, {2, 7, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, 
{4, 7, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 4, 0}, {7, 6, 0}, {0, 5, 2}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, 
{2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {6, 0, 0}, {7, 0, 0}, {7, 3, 0}, 
{0, 0, 2}, {0, 3, 0}, {0, 4, 0}, {1, 3, 0}, {4, 1, 0}, {4, 5, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 7, 0},   // +9.500s 9504
{0, 6, 2}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {2, 7, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, 
{4, 4, 0}, {5, 3, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, 
{3, 0, 0}, {5, 1, 0}, {5, 5, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {3, 6, 0}, 
{4, 1, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 5, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, 
{1, 6, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {6, 2, 0},   // +9.633s 9632
{7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 6, 0}, {0, 7, 0}, {4, 7, 0}, {5, 1, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, 
{6, 1, 0}, {7, 6, 0}, {0, 5, 2}, {1, 3, 0}, {1, 4, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 3, 0}, {3, 6, 0}, 
{3, 7, 0}, {4, 6, 0}, {5, 2, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {2, 2, 0}, {2, 5, 0}, 
{3, 2, 0}, {5, 0, 0}, {5, 7, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {0, 4, 2}, {0, 6, 0}, {0, 7, 0}, {1, 6, 0}, 
{2, 0, 0}, {2, 4, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 6, 0},   // +9.767s 9760
{0, 0, 2}, {1, 0, 0}, {1, 3, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {6, 4, 0}, 
{7, 0, 0}, {7, 3, 0}, {0, 3, 3}, {0, 5, 0}, {1, 7, 0}, {4, 5, 0}, {5, 0, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, 
{6, 3, 0}, {0, 4, 2}, {1, 1, 0}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, 
{3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, 
{3, 0, 0}, {5, 6, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {7, 4, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {1, 7, 0},   // +9.933s 9936
{2, 2, 0}, {2, 6, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, 
{7, 5, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {4, 3, 0}, {6, 6, 0}, 
{7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 4, 0}, {2, 7, 0}, {3, 7, 0}, {4, 7, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, 
{6, 5, 0}, {7, 4, 0}, {7, 6, 0}, {0, 6, 2}, {1, 3, 0}, {1, 5, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, 
{2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {4, 6, 0}, {5, 3, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0},   // +10.067s 10064
{1, 2, 0}, {4, 1, 0}, {4, 4, 0}, {5, 5, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, 
{2, 0, 0}, {2, 5, 0}, {2, 7, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 4, 0}, 
{5, 7, 0}, {7, 6, 0}, {0, 0, 2}, {1, 0, 0}, {1, 3, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {5, 1, 0}, 
{5, 6, 0}, {6, 0, 0}, {7, 0, 0}, {7, 3, 0}, {0, 3, 2}, {0, 6, 0}, {3, 5, 0}, {4, 1, 0}, {4, 3, 0}, {4, 4, 0}, 
{4, 5, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {6, 7, 0}, {0, 5, 2}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0},   // +10.200s 10192
{2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {7, 1, 0}, {0, 0, 3}, {0, 1, 0}, 
{0, 7, 0}, {1, 0, 0}, {4, 6, 0}, {5, 1, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, 
{0, 4, 2}, {0, 6, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, 
{5, 3, 0}, {7, 5, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 1, 0}, {5, 0, 0}, 
{5, 5, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 5, 0}, {0, 7, 0}, {3, 7, 0}, {4, 6, 0},   // +10.333s 10336
{4, 7, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, {0, 4, 2}, {1, 3, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, 
{2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 3, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, 
{0, 3, 0}, {1, 2, 0}, {1, 7, 0}, {4, 4, 0}, {5, 0, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 2, 0}, {7, 2, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, {4, 7, 0}, 
{5, 2, 0}, {7, 0, 0}, {7, 6, 0}, {0, 0, 2}, {1, 0, 0}, {1, 3, 0}, {1, 4, 0}, {2, 3, 0}, {2, 5, 0}, {3, 3, 0},   // +10.467s 10464
{5, 7, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {7, 4, 0}, {0, 3, 2}, {0, 4, 0}, {1, 7, 0}, {3, 5, 0}, {4, 3, 0}, 
{4, 4, 0}, {4, 5, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {0, 1, 2}, {0, 6, 0}, {1, 1, 0}, {1, 5, 0}, {1, 6, 0}, 
{2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {6, 5, 0}, {7, 0, 0}, 
{7, 1, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {2, 7, 0}, {4, 6, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 4, 0}, 
{7, 4, 0}, {7, 5, 0}, {0, 4, 3}, {1, 2, 0}, {1, 4, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0},   // +10.600s 10608
{4, 3, 0}, {4, 5, 0}, {5, 4, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, 
{3, 1, 0}, {4, 1, 0}, {5, 6, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 7, 0}, 
{3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, {7, 6, 0}, {0, 3, 2}, {1, 2, 0}, 
{1, 3, 0}, {1, 4, 0}, {2, 2, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 7, 0}, 
{7, 2, 0}, {7, 3, 0}, {0, 2, 2}, {0, 4, 0}, {4, 1, 0}, {4, 4, 0}, {5, 1, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0},   // +10.733s 10736
{6, 6, 0}, {0, 6, 2}, {1, 0, 0}, {1, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, 
{3, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 3, 0}, {7, 0, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {0, 7, 0}, {1, 3, 0}, 
{2, 3, 0}, {3, 3, 0}, {5, 5, 0}, {6, 0, 0}, {6, 7, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {2, 1, 0}, 
{2, 5, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 1, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {0, 1, 2}, 
{1, 0, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {6, 1, 0},   // +10.867s 10864
{7, 0, 0}, {7, 1, 0}, {0, 0, 2}, {0, 6, 0}, {0, 7, 0}, {3, 6, 0}, {4, 6, 0}, {5, 0, 0}, {5, 3, 0}, {5, 5, 0}, 
{5, 6, 0}, {6, 0, 0}, {7, 5, 0}, {0, 5, 2}, {1, 4, 0}, {1, 5, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, 
{3, 5, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {0, 4, 5}, {0, 6, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 3, 0}, 
{3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {7, 6, 0}, {0, 1, 2}, 
{0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 5, 0}, {1, 7, 0}, {2, 1, 0}, {2, 6, 0}, {3, 2, 0}, {6, 2, 0}, {7, 1, 0},   // +11.033s 11040
{7, 2, 0}, {7, 5, 0}, {0, 5, 2}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 0, 0}, {5, 2, 0}, {5, 5, 0}, 
{6, 1, 0}, {0, 3, 2}, {0, 4, 0}, {1, 3, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 5, 0}, 
{3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {5, 4, 0}, {5, 7, 0}, {7, 3, 0}, {7, 6, 0}, {0, 2, 2}, {1, 2, 0}, {1, 7, 0}, 
{4, 5, 0}, {5, 3, 0}, {5, 6, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {7, 4, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, 
{2, 6, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 2, 0}, {5, 5, 0}, {0, 0, 2},   // +11.200s 11200
{0, 3, 0}, {1, 0, 0}, {1, 3, 0}, {1, 4, 0}, {2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 7, 0}, {3, 0, 0}, {6, 4, 0}, 
{7, 0, 0}, {7, 3, 0}, {0, 4, 2}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, 
{6, 3, 0}, {7, 4, 0}, {0, 6, 2}, {1, 1, 0}, {1, 5, 0}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, 
{3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {6, 5, 0}, {7, 1, 0}, {7, 5, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {2, 0, 0}, 
{2, 7, 0}, {4, 1, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {6, 4, 0}, {7, 0, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0},   // +11.333s 11328
{2, 2, 0}, {2, 5, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, {0, 2, 2}, 
{1, 1, 0}, {1, 2, 0}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {3, 2, 0}, {5, 1, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, 
{7, 2, 0}, {7, 5, 0}, {0, 1, 2}, {0, 6, 0}, {3, 4, 0}, {4, 1, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 2, 0}, 
{5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {0, 5, 3}, {1, 3, 0}, {1, 4, 0}, {1, 5, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, 
{3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {6, 7, 0}, {7, 2, 0}, {7, 3, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {0, 7, 0},   // +11.467s 11472
{1, 2, 0}, {2, 2, 0}, {4, 5, 0}, {5, 1, 0}, {5, 4, 0}, {5, 7, 0}, {6, 6, 0}, {0, 4, 2}, {0, 6, 0}, {1, 6, 0}, 
{2, 4, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 3, 0}, {5, 6, 0}, {7, 0, 0}, 
{0, 0, 2}, {0, 3, 0}, {0, 7, 0}, {1, 0, 0}, {1, 3, 0}, {1, 5, 0}, {2, 0, 0}, {2, 3, 0}, {2, 6, 0}, {3, 0, 0}, 
{5, 0, 0}, {6, 0, 0}, {6, 7, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, 
{5, 4, 0}, {5, 5, 0}, {5, 7, 0}, {7, 5, 0}, {0, 1, 2}, {1, 1, 0}, {1, 4, 0}, {1, 6, 0}, {1, 7, 0}, {2, 1, 0},   // +11.600s 11600
{2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {6, 1, 0}, {7, 0, 0}, {7, 1, 0}, {0, 0, 2}, 
{0, 6, 0}, {1, 0, 0}, {2, 0, 0}, {4, 7, 0}, {5, 0, 0}, {5, 3, 0}, {5, 6, 0}, {6, 0, 0}, {0, 5, 2}, {1, 2, 0}, 
{1, 4, 0}, {1, 5, 0}, {2, 6, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {5, 2, 0}, {5, 5, 0}, {7, 2, 0}, 
{7, 5, 0}, {7, 6, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 7, 0}, {2, 1, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, 
{6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {7, 4, 0}, {0, 4, 2}, {0, 6, 0}, {1, 6, 0}, {2, 4, 0}, {3, 4, 0}, {4, 2, 0},   // +11.733s 11728
{4, 4, 0}, {4, 7, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {0, 3, 2}, {1, 2, 0}, {1, 3, 0}, {1, 5, 0}, 
{2, 2, 0}, {2, 3, 0}, {2, 6, 0}, {2, 7, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, 
{7, 6, 0}, {0, 2, 2}, {0, 5, 0}, {4, 5, 0}, {5, 2, 0}, {5, 5, 0}, {6, 2, 0}, {7, 4, 0}, {0, 4, 3}, {1, 0, 0}, 
{1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, 
{4, 4, 0}, {5, 4, 0}, {5, 7, 0}, {7, 0, 0}, {0, 0, 2}, {0, 3, 0}, {1, 3, 0}, {2, 3, 0}, {2, 7, 0}, {3, 0, 0},   // +11.867s 11872
{4, 1, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 2, 0}, 
{3, 5, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {0, 1, 2}, 
{1, 0, 0}, {1, 1, 0}, {1, 4, 0}, {2, 0, 0}, {2, 5, 0}, {3, 1, 0}, {4, 3, 0}, {6, 5, 0}, {7, 0, 0}, {7, 1, 0}, 
{0, 0, 2}, {0, 4, 0}, {3, 7, 0}, {4, 1, 0}, {4, 7, 0}, {5, 1, 0}, {5, 4, 0}, {5, 7, 0}, {6, 4, 0}, {0, 6, 2}, 
{1, 2, 0}, {1, 5, 0}, {1, 6, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0},   // +12.000s 12000
{4, 6, 0}, {5, 3, 0}, {5, 6, 0}, {7, 2, 0}, {7, 5, 0}, {7, 6, 0}, {0, 1, 2}, {0, 2, 0}, {0, 7, 0}, {1, 1, 0}, 
{4, 4, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, 
{3, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 1, 0}, {5, 2, 0}, {5, 4, 0}, {5, 5, 0}, {5, 7, 0}, {0, 3, 2}, {1, 2, 0}, 
{1, 3, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {5, 0, 0}, {6, 7, 0}, {7, 2, 0}, {7, 3, 0}, 
{7, 6, 0}, {0, 2, 2}, {0, 6, 0}, {0, 7, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 2, 0}, {5, 3, 0},   // +12.133s 12128
{5, 5, 0}, {5, 6, 0}, {6, 6, 0}, {0, 5, 2}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 5, 0}, {2, 6, 0}, 
{3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {7, 0, 0}, {0, 0, 2}, {0, 3, 0}, {1, 3, 0}, {1, 7, 0}, {2, 3, 0}, 
{4, 6, 0}, {5, 0, 0}, {5, 4, 0}, {5, 7, 0}, {6, 0, 0}, {6, 7, 0}, {7, 3, 0}, {0, 4, 3}, {0, 6, 0}, {1, 6, 0}, 
{2, 1, 0}, {2, 4, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 3, 0}, {5, 6, 0}, {7, 5, 0}, 
{0, 1, 2}, {1, 0, 0}, {1, 1, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, {3, 1, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0},   // +12.267s 12272
{7, 1, 0}, {7, 4, 0}, {0, 0, 2}, {0, 5, 0}, {1, 7, 0}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 2, 0}, {5, 4, 0}, 
{5, 5, 0}, {5, 7, 0}, {0, 4, 2}, {1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 5, 0}, 
{3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 2, 0}, {7, 2, 0}, {7, 5, 0}, {7, 6, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, 
{2, 7, 0}, {4, 4, 0}, {5, 3, 0}, {5, 6, 0}, {6, 1, 0}, {7, 1, 0}, {7, 4, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, 
{2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {7, 3, 0}, {7, 6, 0},   // +12.400s 12400
{0, 3, 2}, {1, 2, 0}, {1, 3, 0}, {1, 4, 0}, {2, 2, 0}, {2, 3, 0}, {2, 5, 0}, {3, 3, 0}, {4, 1, 0}, {6, 2, 0}, 
{6, 3, 0}, {7, 2, 0}, {0, 2, 2}, {0, 4, 0}, {2, 7, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 3, 0}, 
{5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {0, 0, 2}, {0, 6, 0}, {1, 0, 0}, {1, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, 
{2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {6, 4, 0}, {7, 0, 0}, {7, 3, 0}, {0, 3, 2}, {0, 5, 0}, 
{1, 3, 0}, {2, 3, 0}, {4, 1, 0}, {4, 6, 0}, {5, 1, 0}, {5, 2, 0}, {5, 5, 0}, {6, 3, 0}, {0, 4, 2}, {1, 4, 0},   // +12.567s 12560
{2, 1, 0}, {2, 5, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 4, 0}, {5, 7, 0}, {7, 1, 0}, 
{7, 5, 0}, {0, 0, 2}, {0, 1, 0}, {0, 7, 0}, {1, 0, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 1, 0}, 
{6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {0, 5, 3}, {0, 6, 0}, {1, 5, 0}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, 
{5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 6, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 5, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 6, 0}, {7, 1, 0}, {7, 2, 0}, {7, 5, 0},   // +12.667s 12672
{0, 1, 2}, {0, 4, 0}, {0, 7, 0}, {4, 4, 0}, {5, 0, 0}, {5, 4, 0}, {5, 7, 0}, {6, 5, 0}, {0, 6, 2}, {1, 3, 0}, 
{1, 5, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 3, 0}, 
{5, 6, 0}, {7, 3, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 7, 0}, {2, 2, 0}, {2, 6, 0}, {3, 3, 0}, 
{6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {2, 5, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, 
{4, 5, 0}, {5, 0, 0}, {5, 2, 0}, {5, 4, 0}, {5, 5, 0}, {5, 7, 0}, {0, 0, 2}, {1, 0, 0}, {1, 3, 0}, {1, 6, 0},   // +12.833s 12832
{2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {6, 0, 0}, {7, 0, 0}, {7, 3, 0}, 
{7, 4, 0}, {0, 3, 2}, {1, 7, 0}, {5, 6, 0}, {6, 7, 0}, {0, 5, 2}, {0, 6, 0}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, 
{2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {4, 6, 0}, 
{5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 1, 0}, {7, 5, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {2, 0, 0}, {2, 7, 0}, 
{6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {7, 4, 0}, {0, 4, 2}, {1, 6, 0}, {3, 3, 0}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0},   // +12.967s 12960
{5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {7, 6, 0}, {0, 2, 2}, {0, 6, 0}, {1, 1, 0}, {1, 2, 0}, {1, 5, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 2, 0}, {3, 6, 0}, {4, 1, 0}, {5, 3, 0}, {6, 2, 0}, {7, 1, 0}, {7, 2, 0}, 
{7, 5, 0}, {0, 1, 3}, {2, 7, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 5, 0}, {6, 1, 0}, {0, 4, 2}, {0, 5, 0}, 
{1, 4, 0}, {1, 6, 0}, {2, 3, 0}, {2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, {3, 7, 0}, {5, 2, 0}, {5, 4, 0}, 
{5, 7, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 4, 0}, {4, 1, 0}, {4, 5, 0},   // +13.100s 13104
{5, 1, 0}, {5, 6, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 6, 2}, {1, 5, 0}, {3, 1, 0}, {3, 5, 0}, 
{4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {1, 4, 0}, 
{2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {7, 0, 0}, {0, 3, 2}, {0, 7, 0}, {1, 3, 0}, 
{4, 5, 0}, {4, 6, 0}, {5, 1, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {0, 4, 2}, 
{0, 6, 0}, {1, 5, 0}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0},   // +13.233s 13232
{5, 3, 0}, {7, 5, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {2, 0, 0}, {2, 6, 0}, {4, 7, 0}, {5, 0, 0}, 
{5, 5, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {7, 1, 0}, {0, 5, 2}, {0, 7, 0}, {3, 3, 0}, {3, 7, 0}, {4, 6, 0}, 
{5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, {0, 2, 2}, {0, 4, 0}, {1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 6, 0}, {6, 6, 0}, {7, 2, 0}, {7, 5, 0}, {0, 1, 2}, {1, 1, 0}, 
{1, 7, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 0, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 5, 0}, {7, 1, 0},   // +13.367s 13360
{0, 5, 3}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, {3, 7, 0}, 
{5, 2, 0}, {7, 3, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 5, 0}, {4, 5, 0}, 
{5, 7, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {7, 4, 0}, {0, 4, 2}, {1, 6, 0}, {1, 7, 0}, {3, 1, 0}, {3, 5, 0}, 
{4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {0, 0, 2}, {0, 6, 0}, {1, 0, 0}, {1, 5, 0}, 
{2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {6, 0, 0}, {7, 0, 0}, {7, 3, 0}, {0, 3, 2},   // +13.533s 13536
{0, 5, 0}, {1, 3, 0}, {2, 7, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 7, 0}, {7, 4, 0}, 
{7, 5, 0}, {0, 1, 2}, {0, 4, 0}, {1, 4, 0}, {1, 6, 0}, {2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, 
{3, 6, 0}, {4, 3, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, {1, 1, 0}, {2, 0, 0}, {2, 4, 0}, {4, 1, 0}, 
{4, 7, 0}, {5, 6, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 7, 0}, {3, 7, 0}, 
{4, 6, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0},   // +13.667s 13664
{2, 2, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 4, 0}, 
{2, 1, 0}, {2, 5, 0}, {4, 1, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 1, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, 
{6, 1, 0}, {0, 6, 2}, {1, 5, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {5, 3, 0}, 
{7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {0, 7, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 6, 0}, {3, 3, 0}, {4, 5, 0}, 
{5, 5, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0},   // +13.800s 13792
{4, 4, 0}, {5, 1, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {1, 0, 3}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 5, 0}, 
{3, 1, 0}, {3, 4, 0}, {7, 0, 0}, {0, 0, 2}, {0, 3, 0}, {0, 6, 0}, {0, 7, 0}, {1, 3, 0}, {2, 3, 0}, {2, 4, 0}, 
{3, 0, 0}, {3, 6, 0}, {4, 6, 0}, {5, 0, 0}, {5, 3, 0}, {5, 6, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {7, 5, 0}, 
{0, 5, 2}, {3, 5, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {5, 5, 0}, {1, 1, 2}, {1, 4, 0}, {1, 5, 0}, {2, 1, 0}, 
{2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {1, 0, 0}, {1, 7, 0}, {2, 0, 0},   // +13.967s 13968
{2, 5, 0}, {3, 7, 0}, {4, 7, 0}, {5, 4, 0}, {5, 7, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {0, 6, 2}, {3, 6, 0}, 
{4, 6, 0}, {5, 0, 0}, {5, 3, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, {1, 2, 2}, {1, 5, 0}, {1, 6, 0}, {2, 2, 0}, 
{2, 4, 0}, {3, 2, 0}, {3, 3, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {0, 5, 0}, {1, 1, 0}, {1, 7, 0}, {2, 1, 0}, 
{2, 6, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, 
{7, 4, 0}, {0, 4, 2}, {3, 7, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, {1, 3, 2}, {1, 4, 0}, {1, 6, 0}, {2, 3, 0},   // +14.133s 14128
{2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {7, 3, 0}, {0, 2, 2}, {0, 3, 0}, {0, 6, 0}, {1, 2, 0}, {2, 2, 0}, {2, 4, 0}, 
{2, 7, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 6, 0}, 
{6, 7, 0}, {7, 2, 0}, {7, 4, 0}, {0, 5, 3}, {3, 4, 0}, {4, 2, 0}, {1, 0, 2}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, 
{2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {6, 0, 0}, {7, 0, 0}, {0, 0, 2}, {0, 3, 0}, {0, 4, 0}, {1, 3, 0}, {2, 3, 0}, 
{2, 5, 0}, {2, 7, 0}, {3, 6, 0}, {4, 1, 0}, {4, 5, 0}, {4, 6, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0},   // +14.267s 14272
{6, 7, 0}, {7, 3, 0}, {7, 5, 0}, {0, 6, 2}, {3, 5, 0}, {4, 3, 0}, {1, 1, 2}, {1, 5, 0}, {1, 6, 0}, {2, 1, 0}, 
{2, 4, 0}, {3, 1, 0}, {3, 2, 0}, {6, 1, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, {0, 5, 0}, {1, 0, 0}, {2, 0, 0}, 
{2, 6, 0}, {3, 7, 0}, {4, 1, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 2, 0}, {5, 4, 0}, {5, 5, 0}, {5, 7, 0}, 
{6, 0, 0}, {7, 0, 0}, {7, 6, 0}, {0, 4, 2}, {3, 6, 0}, {7, 5, 0}, {1, 2, 2}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, 
{2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {0, 6, 0}, {0, 7, 0},   // +14.467s 14464
{1, 1, 0}, {2, 1, 0}, {2, 4, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 1, 0}, {5, 2, 0}, {5, 3, 0}, 
{5, 5, 0}, {5, 6, 0}, {6, 1, 0}, {0, 5, 2}, {3, 7, 0}, {7, 6, 0}, {0, 3, 2}, {1, 3, 0}, {1, 4, 0}, {1, 5, 0}, 
{2, 3, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 2, 2}, {0, 4, 0}, {0, 6, 0}, 
{0, 7, 0}, {1, 2, 0}, {2, 2, 0}, {2, 5, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 0, 0}, {5, 3, 0}, 
{5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 2, 0}, {1, 6, 2}, {3, 4, 0}, {4, 2, 0}, {0, 0, 3}, {1, 0, 0}, {1, 3, 0},   // +14.633s 14640
{1, 5, 0}, {1, 7, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {3, 1, 0}, {6, 4, 0}, {7, 0, 0}, {7, 3, 0}, {0, 3, 2}, 
{0, 4, 0}, {0, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 0, 0}, {5, 2, 0}, {5, 4, 0}, 
{5, 5, 0}, {5, 7, 0}, {6, 3, 0}, {7, 5, 0}, {3, 5, 2}, {4, 3, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {2, 0, 0}, 
{4, 6, 0}, {5, 0, 0}, {5, 2, 0}, {5, 5, 0}, {6, 4, 0}, {7, 5, 0}, {2, 4, 2}, {3, 1, 0}, {3, 6, 0}, {7, 1, 0}, 
{7, 2, 0}, {1, 5, 2}, {2, 0, 0}, {7, 3, 0}, {1, 0, 2}, {1, 4, 0}, {2, 1, 0}, {7, 4, 0}, {1, 1, 2}, {1, 2, 0},   // +14.867s 14864
{2, 2, 0}, {2, 3, 0}, {3, 0, 0}, {5, 5, 0}, {1, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 7, 0}, {3, 1, 0}, {3, 2, 0}, 
{2, 6, 2}, {3, 3, 0}, {4, 2, 0}, {7, 6, 0}, {4, 0, 2}, {4, 3, 0}, {5, 6, 0}, {7, 5, 0}, {3, 4, 2}, {4, 1, 0}, 
{5, 7, 0}, {3, 5, 3}, {3, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {0, 5, 2}, {0, 6, 0}, 
{3, 7, 0}, {0, 4, 2}, {5, 2, 0}, {5, 3, 0}, {5, 4, 2}, {0, 0, 2}, {0, 1, 0}, {0, 2, 2}, {0, 3, 0}, {0, 7, 2}, 
{6, 0, 2}, {6, 1, 2}, {6, 2, 0}, {6, 3, 0}, {6, 4, 2}, {6, 5, 0}, {6, 6, 2}, {6, 7, 3}, {6, 7, 8}, {6, 5, 2},   // +15.567s 15568
{6, 6, 0}, {6, 3, 2}, {6, 4, 0}, {6, 1, 2}, {6, 2, 0}, {0, 7, 2}, {6, 0, 0}, {0, 2, 2}, {0, 3, 0}, {0, 0, 2}, 
{0, 1, 0}, {0, 4, 4}, {5, 3, 0}, {5, 4, 0}, {0, 5, 3}, {0, 6, 0}, {5, 2, 0}, {3, 6, 2}, {3, 7, 0}, {4, 4, 0}, 
{4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {3, 4, 2}, {3, 5, 0}, {4, 1, 0}, {5, 1, 0}, {5, 7, 2}, {2, 6, 2}, {3, 3, 0}, 
{4, 0, 0}, {4, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {1, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 7, 0}, {3, 1, 0}, 
{3, 2, 0}, {4, 2, 0}, {7, 6, 0}, {1, 1, 2}, {1, 2, 0}, {2, 2, 0}, {2, 3, 0}, {3, 0, 0}, {1, 0, 2}, {1, 4, 2},   // +16.100s 16096
{1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {7, 4, 0}, {7, 3, 2}, {7, 1, 2}, {7, 2, 0}, {1, 6, 2}, {1, 7, 0}, {5, 0, 0}, 
{7, 0, 0}, {6, 6, 7}, {6, 7, 0}, {6, 4, 2}, {6, 5, 0}, {6, 2, 2}, {6, 3, 0}, {6, 0, 2}, {6, 1, 0}, {0, 7, 2}, 
{0, 2, 2}, {0, 3, 0}, {0, 0, 2}, {0, 1, 0}, {5, 4, 2}, {0, 4, 2}, {5, 2, 0}, {5, 3, 0}, {0, 5, 2}, {0, 6, 0}, 
{3, 7, 0}, {3, 5, 3}, {3, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {3, 4, 2}, {4, 1, 0}, 
{5, 6, 2}, {5, 7, 0}, {2, 6, 2}, {3, 3, 0}, {4, 0, 0}, {4, 2, 0}, {4, 3, 0}, {5, 5, 0}, {7, 5, 0}, {7, 6, 0},   // +16.733s 16736
{1, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 7, 0}, {3, 2, 0}, {1, 1, 2}, {1, 2, 0}, {2, 2, 0}, {2, 3, 0}, {3, 0, 0}, 
{3, 1, 0}, {1, 0, 2}, {1, 4, 0}, {2, 1, 0}, {1, 5, 2}, {2, 0, 0}, {7, 4, 0}, {7, 3, 2}, {1, 6, 2}, {1, 7, 0}, 
{7, 1, 0}, {7, 2, 0}, {5, 0, 2}, {7, 0, 0}, {1, 6, 7}, {1, 7, 0}, {5, 0, 0}, {7, 0, 2}, {7, 1, 0}, {7, 2, 2}, 
{7, 3, 0}, {1, 4, 2}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {7, 4, 0}, {1, 0, 2}, {1, 2, 0}, {2, 2, 0}, {5, 5, 0}, 
{1, 1, 2}, {1, 3, 0}, {2, 3, 0}, {2, 4, 0}, {3, 0, 0}, {3, 1, 0}, {2, 5, 2}, {2, 7, 0}, {3, 2, 0}, {4, 2, 0},   // +17.267s 17264
{2, 6, 2}, {3, 3, 0}, {4, 0, 0}, {4, 3, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, {5, 7, 2}, {3, 4, 2}, {3, 5, 0}, 
{4, 1, 0}, {5, 1, 0}, {3, 6, 3}, {3, 7, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {0, 4, 2}, {0, 5, 0}, 
{0, 6, 0}, {5, 2, 0}, {5, 3, 2}, {5, 4, 0}, {0, 0, 2}, {0, 1, 2}, {0, 2, 0}, {0, 3, 0}, {0, 7, 2}, {6, 0, 2}, 
{6, 1, 2}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 2}, {6, 6, 0}, {6, 7, 2}, {1, 7, 25}, {3, 3, 0}, {3, 1, 2}, 
{3, 2, 0}, {6, 0, 0}, {1, 4, 3}, {1, 5, 0}, {3, 0, 0}, {6, 1, 0}, {6, 2, 0}, {1, 6, 2}, {1, 0, 2}, {1, 1, 0},   // +18.267s 18272
{1, 2, 2}, {1, 3, 0}, {4, 2, 2}, {4, 3, 0}, {5, 1, 0}, {5, 2, 0}, {5, 3, 0}, {6, 3, 0}, {7, 0, 0}, {5, 4, 2}, 
{3, 4, 2}, {3, 5, 0}, {3, 6, 2}, {3, 7, 0}, {2, 7, 2}, {4, 4, 0}, {6, 4, 0}, {7, 1, 0}, {4, 5, 2}, {4, 6, 0}, 
{0, 0, 2}, {4, 1, 0}, {4, 7, 0}, {6, 5, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, 
{0, 7, 0}, {2, 0, 0}, {4, 0, 0}, {6, 6, 0}, {6, 7, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {2, 1, 2}, {2, 2, 0}, 
{5, 5, 0}, {5, 6, 3}, {5, 7, 0}, {2, 3, 2}, {7, 4, 0}, {0, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 6, 2}, {0, 3, 6},   // +18.833s 18832
{2, 5, 0}, {2, 6, 0}, {0, 2, 2}, {2, 3, 0}, {2, 4, 0}, {7, 4, 2}, {2, 2, 2}, {5, 6, 0}, {5, 7, 0}, {0, 1, 2}, 
{0, 4, 0}, {0, 5, 0}, {0, 6, 0}, {0, 7, 0}, {2, 0, 0}, {2, 1, 0}, {5, 5, 0}, {0, 0, 2}, {4, 0, 0}, {6, 6, 0}, 
{6, 7, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {4, 1, 3}, {4, 6, 0}, {4, 7, 0}, {6, 5, 0}, {7, 2, 0}, {4, 4, 2}, 
{4, 5, 0}, {2, 7, 2}, {3, 7, 0}, {6, 4, 0}, {7, 1, 0}, {3, 5, 2}, {3, 6, 0}, {3, 4, 2}, {5, 3, 2}, {5, 4, 0}, 
{1, 3, 2}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {5, 2, 0}, {6, 3, 0}, {7, 0, 0}, {1, 1, 2}, {1, 2, 0}, {1, 0, 2},   // +19.300s 19296
{1, 5, 2}, {1, 6, 0}, {1, 4, 2}, {3, 0, 0}, {3, 1, 0}, {6, 1, 0}, {6, 2, 0}, {1, 7, 3}, {3, 2, 0}, {3, 3, 0}, 
{6, 0, 0}, {0, 3, 6}, {2, 5, 0}, {2, 6, 0}, {0, 2, 2}, {2, 3, 0}, {2, 4, 0}, {7, 4, 0}, {5, 7, 2}, {0, 5, 2}, 
{0, 6, 0}, {2, 2, 0}, {5, 5, 0}, {5, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 7, 0}, {2, 0, 0}, {2, 1, 0}, 
{6, 7, 0}, {4, 0, 2}, {4, 1, 0}, {4, 7, 0}, {6, 5, 0}, {6, 6, 0}, {7, 2, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, 
{4, 6, 2}, {2, 7, 2}, {4, 4, 0}, {4, 5, 0}, {3, 7, 2}, {6, 4, 0}, {7, 1, 0}, {3, 5, 3}, {3, 6, 0}, {3, 4, 2},   // +19.833s 19840
{5, 3, 2}, {5, 4, 0}, {1, 2, 2}, {1, 3, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {5, 2, 0}, {6, 3, 0}, {7, 0, 0}, 
{1, 0, 2}, {1, 1, 0}, {1, 6, 2}, {1, 4, 2}, {1, 5, 0}, {3, 0, 0}, {3, 1, 2}, {3, 2, 0}, {6, 1, 0}, {6, 2, 0}, 
{1, 7, 2}, {3, 3, 0}, {6, 0, 0}, {1, 7, 6}, {3, 3, 0}, {3, 1, 2}, {3, 2, 0}, {6, 0, 0}, {1, 4, 3}, {1, 5, 0}, 
{3, 0, 0}, {6, 1, 0}, {6, 2, 0}, {1, 6, 2}, {1, 0, 2}, {1, 1, 0}, {1, 2, 2}, {1, 3, 0}, {4, 2, 2}, {4, 3, 0}, 
{5, 1, 0}, {5, 2, 0}, {5, 3, 0}, {6, 3, 0}, {7, 0, 0}, {5, 4, 2}, {3, 4, 2}, {3, 5, 0}, {3, 6, 2}, {3, 7, 0},   // +20.467s 20464
{2, 7, 2}, {4, 4, 0}, {6, 4, 0}, {7, 1, 0}, {4, 5, 2}, {4, 6, 0}, {0, 0, 2}, {4, 1, 0}, {4, 7, 0}, {6, 5, 0}, 
{7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, {0, 7, 0}, {2, 0, 0}, {4, 0, 0}, {6, 6, 0}, 
{6, 7, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {2, 1, 3}, {2, 2, 0}, {5, 5, 0}, {5, 6, 2}, {5, 7, 0}, {2, 3, 2}, 
{7, 4, 0}, {0, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 6, 2}, {2, 7, 2}, {2, 7, 6}, {2, 7, 4}, {2, 7, 7}, {2, 7, 4}, 
{2, 7, 6}, {2, 7, 4}, {2, 7, 7}, {2, 7, 4}, {2, 7, 6}, {1, 2, 2}, {1, 3, 0}, {1, 1, 2}, {2, 5, 0}, {3, 0, 0},   // +21.633s 21632
{3, 2, 0}, {3, 4, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {4, 5, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, 
{0, 6, 2}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, 
{3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 7, 0}, {0, 4, 2}, {0, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 1, 0}, 
{4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {7, 0, 0}, {7, 4, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {7, 1, 0}, {7, 2, 0}, 
{7, 3, 0}, {0, 3, 2}, {0, 7, 0}, {2, 6, 0}, {2, 7, 0}, {5, 2, 0}, {3, 3, 2}, {5, 0, 0}, {6, 0, 0}, {6, 1, 0},   // +21.800s 21792
{6, 2, 0}, {6, 3, 3}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {6, 7, 2}, {6, 7, 20}, {6, 3, 3}, {6, 4, 0}, {6, 5, 0}, 
{6, 6, 0}, {0, 3, 2}, {0, 7, 0}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, {5, 0, 0}, {5, 2, 0}, {6, 0, 0}, {6, 1, 0}, 
{6, 2, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {1, 6, 2}, {5, 3, 0}, {7, 1, 0}, {7, 2, 0}, {7, 3, 0}, {0, 4, 2}, 
{0, 5, 0}, {0, 6, 0}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, 
{3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 4, 0},   // +22.367s 22368
{7, 0, 0}, {7, 4, 0}, {1, 1, 2}, {2, 5, 0}, {3, 0, 0}, {3, 2, 0}, {3, 4, 0}, {4, 1, 0}, {5, 5, 0}, {5, 6, 0}, 
{5, 7, 0}, {1, 2, 2}, {1, 3, 0}, {4, 2, 0}, {4, 3, 0}, {7, 5, 0}, {7, 6, 0}, {1, 2, 19}, {1, 3, 0}, {4, 2, 0}, 
{4, 3, 0}, {7, 5, 0}, {7, 6, 0}, {1, 1, 2}, {2, 5, 0}, {3, 0, 0}, {3, 2, 0}, {3, 4, 0}, {4, 1, 0}, {4, 5, 0}, 
{5, 5, 0}, {5, 6, 0}, {0, 6, 2}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {2, 3, 0}, 
{2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 7, 0}, {0, 4, 2},   // +22.833s 22832
{0, 5, 0}, {1, 6, 0}, {4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {7, 0, 0}, {7, 4, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, 
{7, 1, 0}, {7, 2, 0}, {7, 3, 0}, {0, 3, 2}, {0, 7, 0}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, {5, 0, 2}, {5, 2, 0}, 
{6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {6, 7, 3}, {6, 5, 20}, {6, 6, 0}, 
{6, 7, 0}, {6, 3, 2}, {6, 4, 0}, {0, 3, 2}, {0, 7, 0}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, {5, 0, 0}, {5, 2, 0}, 
{6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {0, 0, 3}, {0, 1, 0}, {0, 2, 0}, {7, 3, 0}, {1, 6, 2}, {5, 3, 0}, {7, 0, 0},   // +23.467s 23472
{7, 1, 0}, {7, 2, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {4, 4, 0}, 
{5, 4, 0}, {7, 4, 0}, {1, 0, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, 
{4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 7, 0}, {1, 1, 2}, {1, 2, 0}, {1, 3, 0}, {2, 5, 0}, {3, 0, 0}, 
{3, 2, 0}, {3, 4, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, {1, 2, 19}, 
{1, 3, 0}, {1, 1, 2}, {2, 5, 0}, {3, 0, 0}, {3, 2, 0}, {3, 4, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {4, 5, 0},   // +23.900s 23904
{5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, {0, 6, 2}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, 
{5, 7, 0}, {0, 4, 2}, {0, 5, 0}, {1, 6, 0}, {4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {7, 0, 0}, {7, 4, 0}, {0, 0, 2}, 
{0, 1, 0}, {0, 2, 0}, {7, 1, 0}, {7, 2, 0}, {7, 3, 0}, {0, 3, 2}, {0, 7, 0}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, 
{5, 2, 0}, {5, 0, 2}, {6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {6, 7, 2},   // +24.133s 24128
{6, 5, 21}, {6, 6, 0}, {6, 7, 0}, {6, 1, 2}, {6, 2, 0}, {6, 3, 0}, {6, 4, 0}, {0, 1, 2}, {0, 2, 0}, {0, 3, 0}, 
{0, 7, 0}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, {5, 0, 0}, {5, 2, 0}, {6, 0, 0}, {0, 0, 2}, {7, 3, 0}, {0, 4, 2}, 
{0, 5, 0}, {1, 6, 0}, {5, 3, 0}, {7, 0, 0}, {7, 1, 0}, {7, 2, 0}, {0, 6, 3}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, 
{2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 4, 0}, 
{4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 4, 0}, {7, 4, 0}, {1, 1, 2}, {1, 2, 0}, {1, 3, 0}, {2, 5, 0},   // +24.667s 24672
{3, 0, 0}, {3, 2, 0}, {3, 4, 0}, {4, 1, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {4, 2, 2}, {4, 3, 0}, {7, 5, 0}, 
{7, 6, 0}, {1, 7, 18}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {7, 2, 0}, {7, 3, 0}, {2, 3, 3}, {2, 4, 0}, {2, 5, 0}, 
{2, 6, 0}, {7, 4, 0}, {4, 0, 2}, {4, 1, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, 
{5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, {0, 2, 2}, {0, 3, 0}, 
{0, 7, 0}, {6, 0, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {3, 7, 2}, {5, 2, 0},   // +25.233s 25232
{5, 3, 0}, {5, 4, 0}, {6, 7, 0}, {2, 7, 2}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {3, 5, 0}, {3, 6, 0}, 
{4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {1, 0, 2}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {3, 0, 0}, {1, 4, 2}, {1, 5, 0}, 
{1, 6, 0}, {5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {1, 6, 21}, {1, 0, 2}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {5, 0, 0}, 
{7, 0, 0}, {7, 1, 0}, {1, 2, 2}, {1, 3, 0}, {3, 0, 0}, {2, 7, 2}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, 
{3, 5, 0}, {3, 6, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {3, 7, 2}, {5, 2, 0}, {5, 3, 0}, {5, 4, 0}, {6, 5, 3},   // +25.833s 25840
{6, 6, 0}, {6, 7, 0}, {0, 7, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 0}, {6, 4, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, 
{0, 3, 0}, {6, 0, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {2, 5, 2}, {2, 6, 0}, {4, 0, 0}, {4, 1, 0}, {4, 4, 0}, 
{4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {2, 1, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {5, 6, 0}, {5, 7, 0}, 
{7, 5, 0}, {7, 6, 0}, {1, 7, 2}, {2, 0, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0}, {1, 4, 33}, {1, 5, 0}, {1, 6, 0}, 
{5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {1, 0, 3}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {3, 0, 0}, {2, 7, 2}, {3, 1, 0},   // +26.633s 26640
{3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {4, 2, 0}, {3, 5, 4}, {3, 6, 0}, {4, 3, 0}, {5, 1, 0}, {3, 7, 2}, {5, 2, 0}, 
{5, 3, 0}, {5, 4, 0}, {6, 7, 2}, {6, 5, 4}, {6, 6, 0}, {0, 7, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 0}, {6, 4, 0}, 
{0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {6, 0, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {2, 5, 2}, {2, 6, 0}, 
{4, 0, 0}, {4, 1, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {2, 1, 3}, {2, 2, 0}, {2, 3, 0}, 
{2, 4, 0}, {5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {1, 7, 2}, {2, 0, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0},   // +27.033s 27040
{1, 7, 20}, {2, 0, 0}, {2, 1, 3}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0}, {2, 5, 2}, 
{2, 6, 0}, {4, 0, 0}, {4, 1, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {0, 4, 2}, 
{0, 5, 0}, {0, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {0, 7, 2}, 
{6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {5, 2, 2}, {5, 3, 0}, {6, 7, 0}, 
{3, 5, 2}, {3, 6, 0}, {3, 7, 0}, {4, 3, 0}, {5, 1, 0}, {5, 4, 0}, {1, 0, 2}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0},   // +27.667s 27664
{2, 7, 0}, {3, 0, 0}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {4, 2, 0}, {1, 4, 2}, {1, 5, 0}, {1, 6, 0}, 
{7, 0, 0}, {7, 1, 0}, {5, 0, 2}, {1, 7, 19}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {7, 2, 0}, {7, 3, 0}, {2, 3, 2}, 
{2, 4, 0}, {2, 5, 0}, {2, 6, 0}, {7, 4, 0}, {4, 0, 2}, {4, 1, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, 
{5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, 
{0, 2, 2}, {0, 3, 0}, {0, 7, 0}, {6, 0, 3}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0},   // +28.233s 28240
{6, 7, 0}, {3, 7, 2}, {5, 2, 0}, {5, 3, 0}, {5, 4, 0}, {2, 7, 2}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, 
{3, 5, 0}, {3, 6, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {1, 0, 2}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {3, 0, 0}, 
{1, 4, 2}, {1, 5, 0}, {1, 6, 0}, {5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {1, 6, 21}, {1, 0, 2}, {1, 1, 0}, {1, 4, 0}, 
{1, 5, 0}, {5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {1, 2, 2}, {1, 3, 0}, {2, 7, 0}, {3, 0, 0}, {3, 1, 2}, {3, 2, 0}, 
{3, 3, 0}, {3, 4, 0}, {3, 5, 0}, {3, 6, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {3, 7, 2}, {5, 2, 0}, {5, 3, 0},   // +28.833s 28832
{5, 4, 0}, {6, 5, 2}, {6, 6, 0}, {6, 7, 0}, {0, 7, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 0}, {6, 4, 0}, {0, 0, 2}, 
{0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {6, 0, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {2, 5, 3}, {2, 6, 0}, {4, 0, 0}, 
{4, 1, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {2, 1, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, 
{5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {1, 7, 2}, {2, 0, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0}, {1, 6, 18}, 
{5, 0, 0}, {1, 0, 3}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {7, 0, 0}, {7, 1, 0}, {1, 2, 2}, {1, 3, 0}, {2, 7, 0},   // +29.433s 29440
{3, 0, 0}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {4, 2, 0}, {3, 5, 2}, {3, 6, 0}, {3, 7, 0}, {4, 3, 0}, 
{5, 1, 0}, {5, 4, 0}, {5, 2, 2}, {5, 3, 0}, {6, 7, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {0, 2, 2}, 
{0, 3, 0}, {0, 7, 0}, {6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, 
{4, 4, 2}, {4, 5, 0}, {4, 6, 0}, {2, 5, 2}, {2, 6, 0}, {4, 0, 0}, {4, 1, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, 
{5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {1, 7, 2}, {2, 1, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {7, 4, 0}, {2, 0, 2},   // +29.733s 29728
{7, 2, 0}, {7, 3, 0}, {1, 7, 21}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {7, 2, 0}, {7, 3, 0}, {2, 3, 2}, {2, 4, 0}, 
{2, 5, 0}, {2, 6, 0}, {7, 4, 0}, {4, 0, 2}, {4, 1, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 5, 0}, 
{5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, {0, 2, 3}, 
{0, 3, 0}, {0, 7, 0}, {6, 0, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {3, 7, 6}, 
{5, 2, 0}, {5, 3, 0}, {5, 4, 0}, {6, 7, 0}, {2, 7, 2}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {3, 5, 0},   // +30.400s 30400
{3, 6, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {1, 0, 6}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {3, 0, 0}, {1, 4, 2}, 
{1, 5, 0}, {1, 6, 0}, {5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {5, 5, 21}, {0, 0, 2}, {0, 5, 0}, {1, 1, 0}, {1, 4, 0}, 
{2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, 
{1, 7, 0}, {5, 0, 0}, {6, 1, 0}, {0, 6, 2}, {1, 5, 0}, {3, 2, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 0, 0}, 
{5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {0, 2, 2}, {0, 5, 0}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0},   // +31.000s 30992
{2, 2, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {5, 2, 0}, {6, 2, 0}, {7, 1, 0}, {7, 2, 0}, 
{7, 4, 0}, {0, 1, 3}, {1, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 7, 0}, {6, 1, 0}, {0, 4, 2}, {0, 6, 0}, {1, 6, 0}, 
{2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, 
{0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {1, 5, 0}, {2, 2, 0}, {2, 6, 0}, {2, 7, 0}, {3, 2, 0}, {4, 4, 0}, 
{5, 5, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 5, 2}, {1, 4, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0},   // +31.133s 31136
{4, 7, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 4, 0}, {0, 0, 2}, {0, 4, 0}, {1, 0, 0}, {1, 6, 0}, {2, 0, 0}, 
{2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 3, 0}, {3, 7, 0}, {4, 1, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 2}, {1, 3, 0}, 
{2, 7, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, 
{4, 2, 0}, {5, 2, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {2, 0, 0}, {2, 5, 0}, {4, 6, 0}, {5, 1, 0},   // +31.267s 31264
{5, 7, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {7, 1, 0}, {0, 4, 2}, {1, 6, 0}, {3, 2, 0}, {3, 6, 0}, {4, 1, 0}, 
{4, 3, 0}, {4, 5, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 5, 0}, {0, 2, 2}, {0, 6, 0}, {1, 2, 0}, {1, 5, 0}, 
{2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {6, 6, 0}, {7, 2, 0}, {0, 1, 2}, {0, 7, 0}, 
{1, 1, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 5, 0}, {7, 1, 0}, {7, 6, 0}, 
{0, 4, 3}, {0, 5, 0}, {1, 4, 0}, {1, 6, 0}, {2, 3, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0},   // +31.400s 31408
{5, 4, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 4, 0}, {4, 4, 0}, 
{5, 0, 0}, {5, 6, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 6, 2}, {0, 7, 0}, {1, 5, 0}, {3, 0, 0}, {3, 4, 0}, 
{4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 6, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {1, 4, 0}, 
{2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {6, 0, 0}, {7, 0, 0}, {7, 3, 0}, {0, 3, 2}, 
{0, 4, 0}, {1, 3, 0}, {1, 7, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 0, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0},   // +31.533s 31536
{6, 7, 0}, {0, 1, 2}, {0, 6, 0}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, 
{5, 3, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, {1, 1, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, {3, 0, 0}, {4, 6, 0}, 
{5, 5, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {7, 4, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {1, 7, 0}, {3, 6, 0}, 
{4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, 
{1, 6, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {6, 2, 0}, {7, 1, 0},   // +31.667s 31664
{7, 2, 0}, {0, 6, 2}, {2, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 1, 0}, {7, 4, 0}, 
{7, 6, 0}, {0, 3, 2}, {0, 5, 0}, {1, 3, 0}, {1, 4, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 3, 0}, {3, 6, 0}, 
{3, 7, 0}, {5, 2, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {1, 2, 0}, {2, 2, 0}, {2, 5, 0}, {2, 7, 0}, {3, 2, 0}, 
{4, 1, 0}, {4, 4, 0}, {5, 7, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {0, 4, 3}, {0, 6, 0}, {1, 6, 0}, {2, 0, 0}, 
{2, 4, 0}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0},   // +31.833s 31840
{1, 0, 0}, {1, 3, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {5, 1, 0}, {6, 4, 0}, 
{7, 0, 0}, {7, 3, 0}, {0, 5, 2}, {3, 5, 0}, {4, 1, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 2, 0}, {5, 5, 0}, 
{5, 7, 0}, {6, 3, 0}, {0, 1, 2}, {0, 4, 0}, {1, 1, 0}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 1, 0}, {2, 4, 0}, 
{2, 5, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {0, 7, 0}, {1, 0, 0}, 
{4, 6, 0}, {5, 1, 0}, {5, 6, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 2, 0},   // +31.967s 31968
{2, 6, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, 
{0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {4, 7, 0}, {5, 0, 0}, 
{6, 6, 0}, {7, 1, 0}, {7, 2, 0}, {0, 4, 2}, {0, 7, 0}, {3, 7, 0}, {4, 6, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, 
{6, 5, 0}, {7, 6, 0}, {0, 3, 2}, {0, 6, 0}, {1, 3, 0}, {1, 5, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, 
{2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {5, 3, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {1, 2, 0}, {1, 7, 0},   // +32.100s 32096
{4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 0, 0}, {5, 2, 0}, {5, 5, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 4, 2}, 
{0, 5, 0}, {1, 4, 0}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, 
{0, 0, 2}, {0, 3, 0}, {1, 0, 0}, {1, 3, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {4, 5, 0}, {5, 6, 0}, 
{6, 0, 0}, {7, 0, 0}, {7, 3, 0}, {7, 4, 0}, {0, 6, 2}, {1, 7, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, 
{5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {6, 7, 0}, {0, 1, 3}, {0, 5, 0}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0},   // +32.233s 32240
{2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {6, 1, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, 
{2, 7, 0}, {4, 5, 0}, {4, 6, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 0, 0}, {7, 0, 0}, {7, 4, 0}, {0, 4, 2}, 
{0, 6, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {5, 3, 0}, {7, 5, 0}, 
{0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 1, 0}, {4, 1, 0}, {4, 7, 0}, 
{5, 5, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {0, 5, 2}, {2, 7, 0}, {3, 7, 0}, {4, 6, 0}, {5, 2, 0},   // +32.367s 32368
{5, 4, 0}, {5, 7, 0}, {7, 6, 0}, {0, 3, 2}, {0, 4, 0}, {1, 3, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, 
{2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {5, 1, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {7, 5, 0}, 
{0, 2, 2}, {1, 2, 0}, {4, 1, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 2, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {5, 2, 0}, {7, 0, 0}, 
{7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {0, 7, 0}, {1, 0, 0}, {1, 3, 0}, {1, 4, 0}, {2, 3, 0}, {2, 5, 0}, {3, 3, 0},   // +32.500s 32496
{4, 5, 0}, {5, 1, 0}, {5, 7, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {0, 4, 2}, {0, 6, 0}, {3, 5, 0}, {4, 2, 0}, 
{4, 3, 0}, {4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {0, 0, 2}, {0, 1, 0}, {1, 1, 0}, {1, 5, 0}, {1, 6, 0}, 
{2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {5, 0, 0}, {6, 5, 0}, {7, 0, 0}, 
{7, 1, 0}, {0, 5, 3}, {0, 7, 0}, {1, 0, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 4, 0}, 
{7, 5, 0}, {0, 2, 2}, {0, 4, 0}, {1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {3, 5, 0},   // +32.633s 32640
{3, 6, 0}, {4, 3, 0}, {5, 4, 0}, {7, 2, 0}, {0, 1, 2}, {1, 1, 0}, {1, 7, 0}, {2, 1, 0}, {2, 4, 0}, {3, 1, 0}, 
{4, 7, 0}, {5, 0, 0}, {5, 6, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {3, 7, 0}, 
{4, 6, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, 
{1, 4, 0}, {2, 2, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 7, 0}, {7, 2, 0}, 
{7, 3, 0}, {7, 4, 0}, {0, 4, 2}, {1, 7, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0},   // +32.767s 32768
{6, 6, 0}, {0, 0, 2}, {0, 6, 0}, {1, 0, 0}, {1, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, 
{3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {5, 3, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 2}, {1, 3, 0}, {2, 7, 0}, {3, 3, 0}, 
{4, 5, 0}, {5, 5, 0}, {6, 0, 0}, {6, 7, 0}, {7, 3, 0}, {7, 4, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {2, 1, 0}, 
{2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {0, 0, 2}, 
{0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {3, 4, 0}, {4, 1, 0}, {6, 1, 0},   // +32.900s 32896
{7, 0, 0}, {7, 1, 0}, {0, 6, 2}, {2, 7, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, 
{6, 0, 0}, {7, 5, 0}, {0, 2, 2}, {0, 5, 0}, {1, 2, 0}, {1, 4, 0}, {1, 5, 0}, {2, 1, 0}, {2, 2, 0}, {2, 5, 0}, 
{2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {5, 2, 0}, {7, 2, 0}, {0, 1, 2}, {1, 1, 0}, {4, 1, 0}, 
{4, 7, 0}, {5, 1, 0}, {5, 7, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {0, 4, 3}, {0, 6, 0}, {1, 6, 0}, {2, 3, 0}, 
{2, 4, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0},   // +33.033s 33040
{0, 2, 2}, {0, 3, 0}, {0, 7, 0}, {1, 2, 0}, {1, 3, 0}, {1, 5, 0}, {2, 2, 0}, {2, 6, 0}, {3, 2, 0}, {4, 4, 0}, 
{6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 5, 2}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, {5, 1, 0}, {5, 2, 0}, {5, 5, 0}, 
{5, 7, 0}, {6, 2, 0}, {0, 0, 2}, {0, 4, 0}, {1, 0, 0}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 3, 0}, {2, 4, 0}, 
{2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {5, 0, 0}, {5, 4, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 2}, {0, 7, 0}, 
{1, 3, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0},   // +33.167s 33168
{0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {5, 2, 0}, 
{0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 4, 0}, {1, 7, 0}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {4, 6, 0}, 
{5, 0, 0}, {5, 7, 0}, {6, 5, 0}, {7, 0, 0}, {7, 1, 0}, {0, 4, 2}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 3, 0}, 
{5, 4, 0}, {5, 6, 0}, {6, 4, 0}, {7, 5, 0}, {0, 2, 2}, {0, 6, 0}, {1, 2, 0}, {1, 5, 0}, {1, 6, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {6, 6, 0}, {7, 2, 0}, {0, 1, 2}, {1, 1, 0},   // +33.333s 33328
{1, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 5, 0}, {7, 1, 0}, {7, 4, 0}, {0, 4, 2}, 
{0, 5, 0}, {1, 4, 0}, {2, 3, 0}, {2, 5, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 4, 0}, {7, 3, 0}, {7, 5, 0}, 
{7, 6, 0}, {0, 2, 3}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {2, 7, 0}, {3, 2, 0}, 
{4, 4, 0}, {5, 6, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {7, 4, 0}, {0, 6, 2}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, 
{5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 6, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0},   // +33.467s 33472
{2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {4, 1, 0}, {6, 0, 0}, {7, 0, 0}, {7, 3, 0}, 
{0, 3, 2}, {1, 3, 0}, {2, 7, 0}, {4, 4, 0}, {4, 5, 0}, {5, 6, 0}, {5, 7, 0}, {6, 7, 0}, {0, 4, 4}, {0, 6, 0}, 
{1, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {5, 3, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, {1, 1, 0}, 
{1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {4, 6, 0}, {5, 1, 0}, 
{5, 5, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {1, 0, 2}, {4, 1, 0}, {4, 5, 0}, {5, 7, 0}, {0, 4, 4}, {0, 5, 0},   // +33.700s 33696
{1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {2, 5, 0}, {3, 2, 0}, {3, 6, 0}, {4, 3, 0}, {5, 2, 0}, {5, 4, 0}, {7, 2, 0}, 
{7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {0, 7, 0}, {1, 1, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, 
{4, 7, 0}, {5, 6, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {4, 6, 2}, {5, 1, 0}, {5, 5, 0}, {0, 6, 3}, {1, 5, 0}, 
{2, 6, 0}, {5, 3, 0}, {7, 6, 0}, {0, 3, 2}, {0, 5, 0}, {1, 2, 0}, {1, 3, 0}, {1, 4, 0}, {2, 2, 0}, {2, 3, 0}, 
{2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 2, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {7, 5, 0},   // +33.833s 33840
{0, 2, 2}, {0, 7, 0}, {4, 4, 0}, {4, 7, 0}, {5, 0, 0}, {5, 6, 0}, {5, 7, 0}, {6, 2, 0}, {0, 4, 2}, {0, 6, 0}, 
{1, 0, 0}, {1, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, 
{5, 3, 0}, {5, 4, 0}, {7, 0, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 3, 0}, {1, 7, 0}, {2, 3, 0}, {2, 6, 0}, 
{4, 5, 0}, {5, 5, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {0, 5, 2}, {1, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 5, 0}, 
{4, 3, 0}, {4, 4, 0}, {5, 0, 0}, {5, 2, 0}, {5, 7, 0}, {0, 1, 2}, {0, 4, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0},   // +34.000s 34000
{2, 1, 0}, {2, 4, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {4, 6, 0}, {5, 4, 0}, {6, 5, 0}, {7, 0, 0}, {7, 1, 0}, 
{0, 0, 2}, {1, 0, 0}, {1, 7, 0}, {4, 5, 0}, {5, 5, 0}, {5, 6, 0}, {6, 4, 0}, {7, 4, 0}, {0, 5, 2}, {0, 6, 0}, 
{1, 2, 0}, {1, 4, 0}, {1, 5, 0}, {2, 2, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, 
{4, 3, 0}, {5, 2, 0}, {5, 3, 0}, {7, 2, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {2, 1, 0}, {2, 7, 0}, 
{4, 6, 0}, {4, 7, 0}, {5, 7, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, {0, 4, 2}, {1, 6, 0}, {3, 3, 0}, {3, 7, 0},   // +34.133s 34128
{5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 4, 0}, {7, 6, 0}, {0, 3, 2}, {0, 6, 0}, {1, 2, 0}, {1, 3, 0}, {1, 5, 0}, 
{2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 2, 0}, {3, 6, 0}, {4, 4, 0}, {5, 5, 0}, {6, 7, 0}, {7, 2, 0}, 
{7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {2, 7, 0}, {4, 1, 0}, {4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 7, 0}, {6, 6, 0}, 
{0, 4, 3}, {0, 5, 0}, {1, 0, 0}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, 
{3, 7, 0}, {5, 4, 0}, {6, 0, 0}, {7, 0, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 3, 0}, {2, 3, 0}, {2, 4, 0},   // +34.267s 34272
{4, 4, 0}, {4, 5, 0}, {5, 1, 0}, {5, 5, 0}, {5, 6, 0}, {6, 7, 0}, {7, 3, 0}, {0, 6, 2}, {1, 5, 0}, {3, 1, 0}, 
{3, 5, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {5, 2, 0}, {5, 3, 0}, {0, 1, 2}, {0, 5, 0}, {1, 0, 0}, {1, 1, 0}, 
{1, 4, 0}, {2, 0, 0}, {2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 6, 0}, {5, 7, 0}, {6, 0, 0}, 
{6, 1, 0}, {7, 0, 0}, {7, 1, 0}, {0, 0, 2}, {0, 7, 0}, {4, 5, 0}, {5, 1, 0}, {5, 4, 0}, {5, 6, 0}, {7, 5, 0}, 
{0, 4, 2}, {0, 6, 0}, {1, 2, 0}, {1, 5, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0},   // +34.400s 34400
{3, 6, 0}, {4, 3, 0}, {5, 3, 0}, {6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {2, 1, 0}, 
{2, 6, 0}, {4, 6, 0}, {4, 7, 0}, {5, 0, 0}, {5, 5, 0}, {5, 7, 0}, {6, 1, 0}, {0, 5, 2}, {0, 7, 0}, {1, 4, 0}, 
{2, 5, 0}, {3, 3, 0}, {3, 7, 0}, {5, 2, 0}, {5, 4, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {0, 3, 2}, {0, 4, 0}, 
{1, 2, 0}, {1, 3, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 2, 0}, {3, 6, 0}, {4, 4, 0}, {5, 6, 0}, 
{6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {0, 2, 2}, {0, 6, 0}, {1, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 0, 0}, {5, 3, 0},   // +34.533s 34528
{5, 5, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, 
{3, 3, 0}, {3, 4, 0}, {3, 7, 0}, {5, 2, 0}, {6, 4, 0}, {7, 0, 0}, {7, 3, 0}, {7, 6, 0}, {0, 3, 3}, {1, 3, 0}, 
{2, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 6, 0}, {5, 7, 0}, {6, 3, 0}, {7, 4, 0}, {0, 4, 2}, {0, 6, 0}, {1, 1, 0}, 
{1, 5, 0}, {1, 6, 0}, {1, 7, 0}, {3, 1, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {5, 3, 0}, {5, 4, 0}, {7, 1, 0}, 
{0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 6, 0},   // +34.667s 34672
{5, 5, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {0, 5, 2}, {1, 4, 0}, {2, 7, 0}, {4, 5, 0}, {5, 2, 0}, {5, 7, 0}, 
{7, 4, 0}, {7, 5, 0}, {0, 2, 2}, {0, 4, 0}, {1, 1, 0}, {1, 2, 0}, {1, 6, 0}, {2, 2, 0}, {2, 5, 0}, {3, 1, 0}, 
{3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {5, 4, 0}, {6, 6, 0}, {7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {2, 1, 0}, 
{2, 4, 0}, {2, 7, 0}, {4, 1, 0}, {4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, {6, 5, 0}, {0, 5, 2}, {0, 6, 0}, 
{1, 4, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 7, 0}, {5, 2, 0}, {5, 3, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2},   // +34.833s 34832
{0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {4, 4, 0}, {5, 1, 0}, 
{5, 7, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {7, 3, 0}, {0, 4, 2}, {1, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 1, 0}, 
{4, 2, 0}, {4, 7, 0}, {5, 4, 0}, {5, 6, 0}, {0, 0, 2}, {0, 6, 0}, {1, 0, 0}, {1, 5, 0}, {2, 0, 0}, {2, 3, 0}, 
{2, 4, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {5, 3, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 2}, {0, 7, 0}, {1, 3, 0}, 
{4, 5, 0}, {5, 1, 0}, {5, 5, 0}, {5, 7, 0}, {6, 0, 0}, {6, 7, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0},   // +34.967s 34960
{1, 6, 0}, {2, 1, 0}, {2, 5, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, 
{5, 2, 0}, {5, 4, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {2, 0, 0}, {2, 4, 0}, {4, 6, 0}, {5, 6, 0}, 
{6, 1, 0}, {7, 0, 0}, {7, 1, 0}, {0, 6, 3}, {0, 7, 0}, {1, 5, 0}, {3, 2, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, 
{5, 0, 0}, {5, 3, 0}, {5, 5, 0}, {6, 0, 0}, {7, 5, 0}, {0, 2, 2}, {0, 5, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 7, 0}, {5, 2, 0}, {7, 2, 0}, {0, 1, 2}, {1, 1, 0},   // +35.100s 35104
{1, 7, 0}, {4, 6, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {0, 4, 2}, {0, 6, 0}, 
{1, 5, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 0, 0}, {5, 3, 0}, 
{7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 6, 0}, {4, 4, 0}, {4, 7, 0}, 
{5, 5, 0}, {5, 7, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 5, 2}, {1, 4, 0}, {1, 7, 0}, {2, 5, 0}, {3, 0, 0}, 
{3, 4, 0}, {4, 2, 0}, {5, 2, 0}, {5, 4, 0}, {6, 2, 0}, {7, 4, 0}, {0, 0, 2}, {0, 4, 0}, {1, 0, 0}, {1, 6, 0},   // +35.233s 35232
{2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {3, 7, 0}, {4, 5, 0}, {5, 6, 0}, {6, 4, 0}, {7, 0, 0}, {7, 6, 0}, 
{0, 3, 2}, {1, 3, 0}, {2, 7, 0}, {4, 4, 0}, {5, 5, 0}, {6, 3, 0}, {7, 3, 0}, {4, 3, 2}, {5, 3, 0}, {7, 4, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, 
{3, 5, 0}, {4, 2, 0}, {5, 2, 0}, {6, 5, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {2, 0, 0}, {2, 5, 0}, 
{2, 7, 0}, {4, 1, 0}, {4, 5, 0}, {4, 6, 0}, {5, 6, 0}, {5, 7, 0}, {6, 4, 0}, {7, 0, 0}, {0, 4, 3}, {1, 6, 0},   // +35.400s 35408
{3, 2, 0}, {3, 6, 0}, {4, 3, 0}, {5, 3, 0}, {5, 4, 0}, {7, 2, 0}, {7, 5, 0}, {0, 2, 2}, {0, 6, 0}, {1, 1, 0}, 
{1, 2, 0}, {1, 5, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 7, 0}, {5, 1, 0}, 
{5, 5, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, {0, 1, 2}, {4, 1, 0}, {4, 6, 0}, {5, 2, 0}, {5, 7, 0}, {7, 6, 0}, 
{0, 3, 2}, {0, 4, 0}, {0, 5, 0}, {1, 4, 0}, {1, 6, 0}, {2, 3, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, 
{3, 7, 0}, {5, 4, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 7, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 4, 0},   // +35.533s 35536
{4, 4, 0}, {5, 1, 0}, {5, 5, 0}, {5, 6, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, 
{3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 3, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 0, 0}, 
{1, 4, 0}, {2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {4, 5, 0}, {5, 7, 0}, {6, 0, 0}, 
{7, 0, 0}, {7, 3, 0}, {0, 4, 2}, {0, 7, 0}, {1, 3, 0}, {1, 6, 0}, {4, 3, 0}, {4, 4, 0}, {5, 0, 0}, {5, 4, 0}, 
{5, 6, 0}, {6, 7, 0}, {0, 1, 2}, {0, 6, 0}, {1, 1, 0}, {1, 5, 0}, {2, 1, 0}, {2, 4, 0}, {3, 0, 0}, {3, 1, 0},   // +35.667s 35664
{3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {5, 3, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, {1, 7, 0}, {2, 0, 0}, {2, 6, 0}, 
{4, 5, 0}, {4, 6, 0}, {5, 5, 0}, {5, 7, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, 
{1, 6, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {3, 6, 0}, {4, 3, 0}, {5, 0, 0}, {5, 2, 0}, {5, 4, 0}, {7, 5, 0}, 
{0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {2, 1, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {4, 7, 0}, {5, 6, 0}, 
{6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {0, 6, 2}, {1, 5, 0}, {1, 7, 0}, {4, 6, 0}, {5, 3, 0}, {5, 5, 0}, {6, 1, 0},   // +35.800s 35792
{7, 4, 0}, {7, 6, 0}, {0, 3, 3}, {0, 5, 0}, {1, 3, 0}, {1, 4, 0}, {2, 2, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, 
{3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 2, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {1, 2, 0}, {2, 7, 0}, 
{4, 4, 0}, {4, 7, 0}, {5, 6, 0}, {5, 7, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {0, 4, 2}, {0, 6, 0}, {1, 5, 0}, 
{1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, {5, 3, 0}, {5, 4, 0}, 
{7, 4, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 0, 0}, {1, 3, 0}, {2, 3, 0}, {2, 6, 0}, {4, 5, 0}, {5, 5, 0},   // +35.933s 35936
{6, 4, 0}, {7, 0, 0}, {7, 3, 0}, {0, 5, 2}, {1, 4, 0}, {2, 7, 0}, {3, 1, 0}, {3, 5, 0}, {4, 1, 0}, {4, 3, 0}, 
{4, 4, 0}, {5, 2, 0}, {5, 7, 0}, {6, 3, 0}, {0, 1, 2}, {0, 4, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0}, {2, 1, 0}, 
{2, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {4, 6, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, 
{4, 1, 0}, {4, 5, 0}, {5, 1, 0}, {5, 5, 0}, {5, 6, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {0, 5, 2}, {0, 6, 0}, 
{1, 4, 0}, {1, 5, 0}, {2, 2, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {5, 2, 0},   // +36.067s 36064
{5, 3, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {2, 1, 0}, {2, 5, 0}, {4, 6, 0}, {4, 7, 0}, 
{5, 7, 0}, {6, 6, 0}, {7, 1, 0}, {7, 2, 0}, {0, 4, 2}, {0, 7, 0}, {1, 6, 0}, {3, 3, 0}, {3, 7, 0}, {5, 1, 0}, 
{5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {6, 5, 0}, {7, 6, 0}, {0, 3, 2}, {0, 6, 0}, {1, 3, 0}, {1, 5, 0}, {2, 2, 0}, 
{2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 2, 0}, {3, 6, 0}, {4, 4, 0}, {5, 5, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 3}, 
{1, 2, 0}, {4, 7, 0}, {5, 0, 0}, {5, 2, 0}, {5, 7, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {0, 4, 2}, {0, 5, 0},   // +36.233s 36240
{0, 7, 0}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, {3, 7, 0}, {4, 2, 0}, 
{5, 4, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 0, 0}, {1, 3, 0}, {1, 7, 0}, {2, 3, 0}, {2, 4, 0}, {4, 4, 0}, 
{4, 5, 0}, {5, 5, 0}, {5, 6, 0}, {6, 0, 0}, {6, 7, 0}, {7, 0, 0}, {7, 3, 0}, {0, 6, 2}, {1, 5, 0}, {3, 1, 0}, 
{3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {5, 0, 0}, {5, 2, 0}, {5, 3, 0}, {0, 1, 2}, {0, 5, 0}, {1, 1, 0}, {1, 4, 0}, 
{2, 0, 0}, {2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 6, 0}, {5, 7, 0}, {6, 1, 0}, {7, 1, 0},   // +36.333s 36336
{0, 0, 2}, {1, 0, 0}, {1, 7, 0}, {4, 5, 0}, {5, 4, 0}, {5, 6, 0}, {6, 0, 0}, {7, 0, 0}, {7, 4, 0}, {7, 5, 0}, 
{0, 4, 2}, {0, 6, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, 
{5, 3, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {2, 7, 0}, 
{4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {5, 7, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {0, 5, 2}, {1, 4, 0}, {3, 3, 0}, 
{3, 7, 0}, {5, 2, 0}, {5, 4, 0}, {7, 4, 0}, {7, 5, 0}, {7, 6, 0}, {0, 3, 2}, {0, 4, 0}, {1, 3, 0}, {1, 6, 0},   // +36.500s 36496
{2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 6, 0}, {4, 4, 0}, {5, 6, 0}, {6, 3, 0}, {7, 2, 0}, 
{7, 3, 0}, {0, 2, 2}, {0, 6, 0}, {1, 2, 0}, {2, 7, 0}, {4, 1, 0}, {4, 2, 0}, {4, 7, 0}, {5, 3, 0}, {5, 5, 0}, 
{6, 2, 0}, {0, 0, 2}, {0, 5, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, 
{3, 7, 0}, {5, 2, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 3}, {1, 0, 0}, {1, 3, 0}, {2, 3, 0}, {2, 5, 0}, {4, 4, 0}, 
{4, 5, 0}, {5, 1, 0}, {5, 6, 0}, {5, 7, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, {0, 4, 2}, {0, 6, 0}, {1, 5, 0},   // +36.633s 36640
{1, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {5, 3, 0}, {5, 4, 0}, {0, 0, 2}, {0, 1, 0}, 
{1, 0, 0}, {1, 1, 0}, {2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 6, 0}, {5, 5, 0}, 
{6, 5, 0}, {7, 0, 0}, {7, 1, 0}, {0, 5, 2}, {0, 7, 0}, {1, 4, 0}, {4, 5, 0}, {5, 1, 0}, {5, 2, 0}, {5, 7, 0}, 
{6, 4, 0}, {7, 5, 0}, {0, 2, 2}, {0, 4, 0}, {1, 2, 0}, {1, 6, 0}, {2, 2, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, 
{3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {5, 4, 0}, {7, 2, 0}, {0, 1, 2}, {1, 1, 0}, {2, 1, 0}, {2, 4, 0}, {4, 6, 0},   // +36.767s 36768
{4, 7, 0}, {5, 0, 0}, {5, 5, 0}, {5, 6, 0}, {6, 5, 0}, {6, 6, 0}, {7, 1, 0}, {0, 5, 2}, {0, 6, 0}, {0, 7, 0}, 
{1, 4, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {5, 2, 0}, {5, 3, 0}, {7, 5, 0}, {7, 6, 0}, 
{0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {1, 7, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {3, 6, 0}, {4, 4, 0}, 
{5, 7, 0}, {6, 7, 0}, {7, 2, 0}, {7, 3, 0}, {0, 4, 2}, {1, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, 
{5, 0, 0}, {5, 4, 0}, {5, 6, 0}, {6, 6, 0}, {0, 0, 2}, {0, 6, 0}, {1, 0, 0}, {1, 5, 0}, {2, 0, 0}, {2, 3, 0},   // +36.900s 36896
{2, 4, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {5, 3, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 2}, {1, 3, 0}, {1, 7, 0}, 
{4, 4, 0}, {4, 5, 0}, {5, 5, 0}, {5, 7, 0}, {6, 0, 0}, {6, 7, 0}, {7, 3, 0}, {7, 4, 0}, {0, 4, 2}, {0, 5, 0}, 
{1, 4, 0}, {1, 6, 0}, {2, 1, 0}, {2, 5, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, 
{5, 2, 0}, {5, 4, 0}, {0, 0, 3}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {2, 0, 0}, {2, 4, 0}, {2, 7, 0}, {4, 6, 0}, 
{5, 6, 0}, {6, 1, 0}, {7, 0, 0}, {7, 1, 0}, {0, 6, 2}, {1, 5, 0}, {3, 2, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0},   // +37.033s 37040
{5, 3, 0}, {5, 5, 0}, {6, 0, 0}, {7, 4, 0}, {7, 5, 0}, {0, 2, 2}, {0, 5, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 7, 0}, {5, 2, 0}, {7, 2, 0}, {0, 1, 2}, {1, 1, 0}, 
{2, 7, 0}, {4, 1, 0}, {4, 6, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 1, 0}, {6, 2, 0}, {7, 1, 0}, {0, 4, 2}, 
{0, 6, 0}, {1, 5, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 3, 0}, 
{7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 6, 0}, {4, 4, 0}, {4, 7, 0},   // +37.167s 37168
{5, 1, 0}, {5, 5, 0}, {5, 7, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 5, 2}, {1, 4, 0}, {3, 0, 0}, {3, 4, 0}, 
{4, 1, 0}, {4, 2, 0}, {5, 2, 0}, {5, 4, 0}, {6, 2, 0}, {0, 0, 2}, {0, 4, 0}, {1, 0, 0}, {1, 6, 0}, {2, 0, 0}, 
{2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 3, 0}, {3, 7, 0}, {4, 5, 0}, {5, 6, 0}, {6, 4, 0}, {7, 0, 0}, {7, 6, 0}, 
{0, 3, 2}, {0, 7, 0}, {1, 3, 0}, {4, 3, 0}, {4, 4, 0}, {5, 1, 0}, {5, 3, 0}, {5, 5, 0}, {6, 3, 0}, {7, 3, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0},   // +37.300s 37296
{4, 2, 0}, {5, 2, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {2, 0, 0}, {2, 5, 0}, {4, 5, 0}, {4, 6, 0}, 
{5, 0, 0}, {5, 6, 0}, {5, 7, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {7, 1, 0}, {0, 4, 2}, {0, 7, 0}, {1, 6, 0}, 
{3, 2, 0}, {3, 6, 0}, {4, 3, 0}, {5, 3, 0}, {5, 4, 0}, {7, 5, 0}, {0, 2, 3}, {0, 6, 0}, {1, 2, 0}, {1, 5, 0}, 
{2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 7, 0}, {5, 5, 0}, {6, 6, 0}, {7, 1, 0}, 
{7, 2, 0}, {0, 1, 2}, {1, 1, 0}, {1, 7, 0}, {4, 6, 0}, {5, 0, 0}, {5, 2, 0}, {5, 7, 0}, {6, 5, 0}, {7, 6, 0},   // +37.433s 37440
{0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {1, 6, 0}, {2, 3, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, 
{5, 4, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 4, 0}, {4, 4, 0}, 
{4, 7, 0}, {5, 5, 0}, {5, 6, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, {7, 4, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, 
{1, 7, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {5, 2, 0}, {5, 3, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 0, 0}, 
{1, 4, 0}, {2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {4, 5, 0}, {5, 7, 0}, {6, 0, 0},   // +37.567s 37568
{7, 0, 0}, {7, 3, 0}, {0, 4, 2}, {1, 3, 0}, {1, 6, 0}, {2, 7, 0}, {4, 3, 0}, {4, 4, 0}, {5, 4, 0}, {5, 6, 0}, 
{6, 7, 0}, {7, 4, 0}, {0, 1, 2}, {0, 6, 0}, {1, 1, 0}, {1, 5, 0}, {2, 1, 0}, {2, 4, 0}, {3, 0, 0}, {3, 1, 0}, 
{3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {5, 3, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, {2, 0, 0}, {2, 6, 0}, {4, 1, 0}, 
{4, 5, 0}, {4, 6, 0}, {5, 5, 0}, {5, 7, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, 
{1, 6, 0}, {2, 7, 0}, {3, 2, 0}, {3, 6, 0}, {4, 3, 0}, {5, 2, 0}, {5, 4, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0},   // +37.733s 37728
{1, 1, 0}, {1, 2, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {4, 7, 0}, {5, 6, 0}, 
{6, 2, 0}, {7, 1, 0}, {7, 2, 0}, {0, 6, 2}, {1, 5, 0}, {4, 1, 0}, {4, 6, 0}, {5, 1, 0}, {5, 3, 0}, {5, 5, 0}, 
{6, 1, 0}, {7, 6, 0}, {0, 3, 2}, {0, 5, 0}, {1, 3, 0}, {1, 4, 0}, {2, 2, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, 
{3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 2, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 3}, {0, 7, 0}, {1, 2, 0}, 
{4, 4, 0}, {4, 7, 0}, {5, 6, 0}, {5, 7, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {0, 4, 2}, {0, 6, 0}, {1, 5, 0},   // +37.867s 37872
{1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {5, 1, 0}, {5, 3, 0}, {5, 4, 0}, {7, 6, 0}, 
{0, 0, 2}, {0, 3, 0}, {1, 0, 0}, {1, 3, 0}, {2, 3, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {4, 5, 0}, {5, 5, 0}, 
{6, 4, 0}, {7, 0, 0}, {7, 3, 0}, {0, 5, 2}, {0, 7, 0}, {1, 4, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, 
{5, 0, 0}, {5, 2, 0}, {5, 7, 0}, {6, 3, 0}, {0, 1, 2}, {0, 4, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0}, {2, 1, 0}, 
{2, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, {1, 7, 0},   // +38.000s 38000
{4, 5, 0}, {4, 6, 0}, {5, 5, 0}, {5, 6, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0}, {0, 5, 2}, {0, 6, 0}, {1, 4, 0}, 
{1, 5, 0}, {2, 2, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {5, 0, 0}, {5, 2, 0}, 
{5, 3, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {2, 1, 0}, {2, 5, 0}, {7, 1, 0}, {7, 2, 0}, 
{0, 4, 2}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 4, 0}, {5, 7, 0}, {0, 3, 2}, {0, 6, 0}, {1, 3, 0}, {1, 5, 0}, 
{1, 6, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {5, 3, 0}, {5, 6, 0},   // +38.133s 38128
{7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {1, 2, 0}, {1, 7, 0}, {4, 4, 0}, {5, 2, 0}, {5, 5, 0}, {6, 5, 0}, 
{6, 6, 0}, {7, 2, 0}, {7, 4, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, 
{3, 7, 0}, {4, 2, 0}, {4, 7, 0}, {5, 4, 0}, {5, 7, 0}, {0, 0, 3}, {0, 3, 0}, {1, 0, 0}, {1, 3, 0}, {1, 6, 0}, 
{2, 3, 0}, {2, 4, 0}, {2, 7, 0}, {3, 3, 0}, {6, 7, 0}, {7, 0, 0}, {7, 3, 0}, {7, 6, 0}, {0, 6, 2}, {3, 5, 0}, 
{4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 6, 0}, {7, 4, 0}, {0, 1, 2},   // +38.300s 38304
{0, 5, 0}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, 
{4, 2, 0}, {6, 0, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, {2, 0, 0}, {2, 7, 0}, {4, 1, 0}, {4, 6, 0}, {5, 4, 0}, 
{5, 7, 0}, {6, 7, 0}, {7, 0, 0}, {0, 4, 2}, {0, 6, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 2, 0}, {3, 5, 0}, 
{3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 3, 0}, {5, 6, 0}, {7, 2, 0}, {7, 5, 0}, {0, 2, 2}, {1, 1, 0}, {1, 2, 0}, 
{1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 1, 0}, {5, 1, 0}, {6, 0, 0}, {6, 1, 0}, {7, 1, 0}, {0, 1, 2}, {0, 5, 0},   // +38.433s 38432
{3, 7, 0}, {4, 1, 0}, {4, 6, 0}, {4, 7, 0}, {5, 2, 0}, {5, 4, 0}, {5, 5, 0}, {5, 7, 0}, {7, 6, 0}, {0, 4, 2}, 
{1, 3, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, 
{6, 2, 0}, {7, 2, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, {0, 6, 0}, {0, 7, 0}, {1, 2, 0}, {4, 4, 0}, 
{5, 1, 0}, {5, 3, 0}, {5, 6, 0}, {6, 1, 0}, {0, 5, 2}, {1, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, 
{4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {7, 0, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {0, 7, 0}, {1, 0, 0},   // +38.567s 38560
{1, 3, 0}, {1, 4, 0}, {2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {3, 3, 0}, {5, 0, 0}, {6, 2, 0}, {6, 3, 0}, {7, 3, 0}, 
{0, 4, 2}, {0, 6, 0}, {1, 6, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, 
{5, 7, 0}, {0, 1, 3}, {1, 0, 0}, {1, 1, 0}, {1, 5, 0}, {1, 7, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, 
{3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {6, 4, 0}, {7, 0, 0}, {7, 1, 0}, {0, 0, 2}, {0, 5, 0}, {2, 0, 0}, {4, 6, 0}, 
{5, 0, 0}, {5, 2, 0}, {5, 5, 0}, {6, 3, 0}, {0, 4, 2}, {1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, {2, 5, 0},   // +38.700s 38704
{3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 4, 0}, {5, 7, 0}, {7, 2, 0}, {7, 5, 0}, {0, 1, 2}, 
{0, 2, 0}, {1, 1, 0}, {1, 7, 0}, {2, 1, 0}, {2, 4, 0}, {3, 1, 0}, {6, 4, 0}, {6, 5, 0}, {7, 1, 0}, {7, 4, 0}, 
{0, 2, 2}, {0, 5, 0}, {1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {1, 7, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {3, 6, 0}, 
{4, 6, 0}, {5, 0, 0}, {5, 2, 0}, {5, 5, 0}, {6, 5, 0}, {7, 0, 0}, {7, 2, 0}, {7, 4, 0}, {7, 5, 0}, {7, 1, 2}, 
{7, 2, 0}, {1, 5, 2}, {2, 0, 0}, {7, 3, 0}, {1, 0, 2}, {1, 4, 0}, {2, 1, 0}, {7, 4, 0}, {1, 1, 2}, {1, 2, 0},   // +38.900s 38896
{2, 2, 0}, {2, 3, 0}, {3, 0, 0}, {5, 5, 0}, {1, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 7, 0}, {3, 1, 0}, {3, 2, 0}, 
{2, 6, 2}, {3, 3, 0}, {4, 2, 0}, {7, 6, 0}, {4, 0, 2}, {4, 3, 0}, {5, 6, 0}, {7, 5, 0}, {3, 4, 3}, {4, 1, 0}, 
{5, 7, 0}, {3, 5, 2}, {3, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {0, 5, 2}, {0, 6, 0}, 
{3, 7, 0}, {0, 4, 2}, {5, 2, 0}, {5, 3, 0}, {5, 4, 2}, {0, 0, 2}, {0, 1, 0}, {0, 2, 2}, {0, 3, 0}, {0, 7, 2}, 
{6, 0, 2}, {6, 1, 0}, {6, 2, 2}, {6, 3, 0}, {6, 4, 2}, {6, 5, 0}, {6, 6, 3}, {6, 7, 0}, {6, 7, 10}, {6, 5, 2},   // +39.600s 39600
{6, 6, 0}, {6, 3, 2}, {6, 4, 0}, {6, 1, 2}, {6, 2, 0}, {0, 7, 2}, {6, 0, 0}, {0, 2, 2}, {0, 3, 0}, {0, 0, 2}, 
{0, 1, 0}, {0, 4, 5}, {5, 3, 0}, {5, 4, 0}, {0, 5, 2}, {0, 6, 0}, {5, 2, 0}, {3, 5, 2}, {3, 6, 0}, {3, 7, 0}, 
{4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {3, 4, 2}, {4, 1, 0}, {5, 1, 0}, {5, 7, 2}, {2, 6, 2}, {3, 3, 0}, 
{4, 0, 0}, {4, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {1, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 7, 0}, {3, 1, 0}, 
{3, 2, 0}, {4, 2, 0}, {7, 6, 0}, {1, 1, 2}, {1, 2, 0}, {2, 2, 0}, {2, 3, 0}, {3, 0, 0}, {1, 0, 2}, {1, 4, 0},   // +40.100s 40096
{2, 1, 0}, {1, 5, 2}, {2, 0, 0}, {7, 4, 0}, {7, 3, 2}, {1, 6, 2}, {1, 7, 0}, {7, 1, 0}, {7, 2, 0}, {5, 0, 3}, 
{7, 0, 0}, {6, 6, 6}, {6, 7, 0}, {6, 4, 2}, {6, 5, 0}, {6, 2, 2}, {6, 3, 0}, {6, 0, 2}, {6, 1, 0}, {0, 7, 2}, 
{0, 1, 2}, {0, 2, 0}, {0, 3, 0}, {0, 0, 2}, {5, 4, 2}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {5, 2, 0}, {5, 3, 0}, 
{3, 7, 3}, {3, 5, 2}, {3, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {3, 4, 2}, {4, 1, 0}, 
{2, 6, 2}, {5, 6, 0}, {5, 7, 0}, {2, 5, 2}, {2, 7, 0}, {3, 3, 0}, {4, 0, 0}, {4, 2, 0}, {4, 3, 0}, {5, 5, 0},   // +40.767s 40768
{7, 5, 0}, {7, 6, 0}, {1, 1, 2}, {1, 2, 0}, {1, 3, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, {3, 2, 0}, {2, 2, 2}, 
{3, 0, 0}, {1, 0, 2}, {1, 4, 2}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {7, 4, 2}, {7, 2, 2}, {7, 3, 0}, {1, 6, 3}, 
{1, 7, 0}, {7, 0, 0}, {7, 1, 0}, {5, 0, 2}, {1, 6, 6}, {1, 7, 0}, {5, 0, 0}, {7, 0, 0}, {7, 1, 2}, {7, 2, 0}, 
{1, 5, 2}, {2, 0, 0}, {7, 3, 0}, {1, 0, 2}, {1, 4, 0}, {2, 1, 0}, {5, 5, 0}, {7, 4, 0}, {1, 1, 2}, {1, 2, 0}, 
{2, 2, 0}, {2, 3, 0}, {3, 0, 0}, {1, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 7, 0}, {3, 1, 0}, {3, 2, 0}, {2, 6, 2},   // +41.333s 41328
{3, 3, 0}, {4, 0, 0}, {4, 2, 0}, {4, 3, 0}, {7, 6, 0}, {5, 6, 2}, {5, 7, 0}, {7, 5, 0}, {3, 4, 2}, {4, 1, 0}, 
{3, 5, 3}, {3, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {3, 7, 2}, {0, 4, 2}, {0, 5, 0}, 
{0, 6, 0}, {5, 2, 0}, {5, 3, 0}, {5, 4, 2}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {0, 3, 2}, {0, 7, 2}, {6, 0, 2}, 
{6, 1, 0}, {6, 2, 2}, {6, 3, 0}, {6, 4, 2}, {6, 5, 0}, {6, 6, 2}, {6, 7, 0}, {1, 7, 28}, {3, 2, 0}, {3, 3, 0}, 
{1, 4, 2}, {3, 0, 0}, {3, 1, 0}, {6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {1, 5, 2}, {1, 6, 0}, {1, 0, 2}, {1, 1, 2},   // +42.333s 42336
{1, 2, 0}, {1, 3, 2}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {5, 2, 0}, {6, 3, 0}, {7, 0, 0}, {5, 3, 2}, {5, 4, 0}, 
{3, 4, 2}, {3, 5, 2}, {3, 6, 0}, {2, 7, 2}, {3, 7, 0}, {6, 4, 0}, {7, 1, 0}, {4, 4, 2}, {4, 5, 0}, {4, 6, 2}, 
{4, 7, 0}, {0, 0, 3}, {0, 1, 0}, {0, 4, 0}, {0, 7, 0}, {4, 0, 0}, {4, 1, 0}, {6, 5, 0}, {6, 6, 0}, {7, 2, 0}, 
{7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {0, 5, 0}, {0, 6, 0}, {2, 0, 0}, {2, 1, 0}, {6, 7, 0}, {2, 2, 2}, 
{5, 5, 0}, {5, 6, 0}, {5, 7, 2}, {2, 3, 2}, {2, 4, 0}, {2, 5, 0}, {7, 4, 0}, {0, 3, 2}, {2, 6, 0}, {0, 3, 6},   // +42.867s 42864
{2, 6, 0}, {2, 4, 2}, {2, 5, 0}, {0, 2, 2}, {2, 3, 0}, {7, 4, 0}, {5, 7, 2}, {0, 5, 3}, {0, 6, 0}, {2, 1, 0}, 
{2, 2, 0}, {5, 5, 0}, {5, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 7, 0}, {2, 0, 0}, {4, 0, 0}, {6, 7, 0}, 
{4, 1, 2}, {4, 7, 0}, {6, 5, 0}, {6, 6, 0}, {7, 2, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {4, 5, 2}, {4, 6, 0}, 
{2, 7, 2}, {4, 4, 0}, {3, 6, 2}, {3, 7, 0}, {6, 4, 0}, {7, 1, 0}, {3, 4, 2}, {3, 5, 0}, {5, 4, 2}, {4, 2, 2}, 
{4, 3, 0}, {5, 1, 0}, {5, 2, 0}, {5, 3, 0}, {6, 3, 0}, {1, 2, 2}, {1, 3, 0}, {7, 0, 0}, {1, 0, 2}, {1, 1, 0},   // +43.333s 43328
{1, 6, 2}, {1, 4, 2}, {1, 5, 0}, {3, 0, 0}, {6, 2, 0}, {3, 1, 3}, {3, 2, 0}, {6, 0, 0}, {6, 1, 0}, {1, 7, 2}, 
{3, 3, 0}, {0, 3, 4}, {2, 6, 0}, {0, 2, 2}, {2, 4, 0}, {2, 5, 0}, {2, 3, 2}, {7, 4, 0}, {5, 7, 2}, {5, 6, 2}, 
{0, 1, 2}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, {0, 7, 0}, {2, 1, 0}, {2, 2, 0}, {5, 5, 0}, {0, 0, 2}, {2, 0, 0}, 
{4, 0, 0}, {6, 6, 0}, {6, 7, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {4, 1, 2}, {4, 7, 0}, {6, 5, 0}, {7, 2, 0}, 
{4, 5, 3}, {4, 6, 0}, {2, 7, 4}, {3, 7, 0}, {4, 4, 0}, {6, 4, 0}, {7, 1, 0}, {3, 5, 2}, {3, 6, 0}, {3, 4, 2},   // +43.933s 43936
{5, 4, 2}, {1, 3, 2}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {5, 2, 0}, {5, 3, 0}, {6, 3, 0}, {7, 0, 0}, {1, 1, 2}, 
{1, 2, 0}, {1, 0, 2}, {1, 5, 2}, {1, 6, 0}, {1, 4, 2}, {3, 0, 0}, {3, 1, 0}, {6, 2, 0}, {1, 7, 2}, {3, 2, 0}, 
{3, 3, 0}, {6, 0, 0}, {6, 1, 0}, {1, 7, 9}, {3, 2, 0}, {3, 3, 0}, {6, 0, 0}, {1, 4, 2}, {3, 0, 0}, {3, 1, 0}, 
{6, 1, 0}, {6, 2, 0}, {1, 5, 2}, {1, 6, 0}, {1, 0, 2}, {1, 1, 2}, {1, 2, 0}, {1, 3, 2}, {4, 2, 0}, {4, 3, 0}, 
{5, 1, 0}, {5, 2, 0}, {6, 3, 0}, {7, 0, 0}, {5, 3, 2}, {5, 4, 0}, {3, 4, 2}, {3, 5, 2}, {3, 6, 0}, {2, 7, 3},   // +44.600s 44608
{3, 7, 0}, {6, 4, 0}, {7, 1, 0}, {4, 4, 2}, {4, 5, 0}, {4, 6, 2}, {4, 7, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, 
{0, 7, 0}, {4, 0, 0}, {4, 1, 0}, {6, 5, 0}, {6, 6, 0}, {7, 2, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2}, 
{0, 5, 0}, {0, 6, 0}, {2, 0, 0}, {2, 1, 0}, {6, 7, 0}, {2, 2, 2}, {5, 5, 0}, {5, 6, 0}, {2, 3, 2}, {5, 7, 0}, 
{2, 4, 2}, {2, 5, 0}, {7, 4, 0}, {0, 3, 2}, {2, 6, 0}, {2, 7, 4}, {2, 7, 5}, {2, 7, 4}, {2, 7, 6}, {2, 7, 4}, 
{2, 7, 6}, {2, 7, 5}, {2, 7, 6}, {2, 7, 4}, {2, 7, 6}, {1, 1, 4}, {1, 2, 0}, {1, 3, 0}, {2, 5, 0}, {4, 2, 0},   // +45.733s 45728
{4, 3, 0}, {7, 5, 0}, {7, 6, 0}, {1, 0, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 0, 0}, {3, 2, 0}, {3, 4, 0}, 
{4, 1, 0}, {4, 5, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, 
{1, 6, 0}, {2, 0, 0}, {2, 1, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 4, 0}, {4, 6, 0}, {4, 7, 0}, 
{5, 1, 0}, {5, 4, 0}, {7, 4, 0}, {0, 0, 3}, {5, 3, 0}, {7, 0, 0}, {7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, 
{0, 3, 0}, {7, 3, 0}, {0, 7, 2}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, {5, 0, 0}, {5, 2, 0}, {6, 0, 0}, {6, 1, 2},   // +45.933s 45936
{6, 2, 0}, {6, 3, 0}, {6, 4, 0}, {6, 5, 2}, {6, 6, 0}, {6, 7, 0}, {6, 7, 21}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, 
{6, 6, 0}, {0, 7, 2}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, {5, 0, 0}, {6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {0, 1, 2}, 
{0, 2, 0}, {0, 3, 0}, {5, 2, 0}, {0, 0, 2}, {7, 1, 0}, {7, 2, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, 
{1, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 1, 0}, {4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {7, 0, 0}, {7, 4, 0}, {0, 6, 2}, 
{1, 0, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 5, 0}, {4, 6, 0},   // +46.500s 46496
{4, 7, 0}, {5, 1, 0}, {5, 7, 0}, {1, 1, 2}, {1, 2, 0}, {1, 3, 0}, {2, 5, 0}, {3, 0, 0}, {3, 2, 0}, {3, 4, 0}, 
{4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, {1, 2, 19}, {1, 3, 0}, {1, 1, 2}, 
{2, 5, 0}, {3, 0, 0}, {3, 2, 0}, {3, 4, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {4, 5, 0}, {5, 5, 0}, {5, 6, 0}, 
{7, 5, 0}, {7, 6, 0}, {1, 0, 2}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, 
{3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 7, 0}, {0, 4, 2}, {0, 5, 0},   // +46.933s 46928
{0, 6, 0}, {1, 6, 0}, {4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {7, 0, 0}, {7, 4, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, 
{7, 1, 0}, {7, 2, 0}, {7, 3, 0}, {0, 3, 3}, {0, 7, 0}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, {5, 2, 0}, {5, 0, 2}, 
{6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {6, 7, 2}, {6, 5, 21}, {6, 6, 0}, 
{6, 7, 0}, {6, 1, 2}, {6, 2, 0}, {6, 3, 0}, {6, 4, 0}, {0, 7, 2}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, {5, 0, 0}, 
{5, 2, 0}, {6, 0, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {7, 3, 0}, {1, 6, 2}, {5, 3, 0}, {7, 0, 0},   // +47.567s 47568
{7, 1, 0}, {7, 2, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, 
{2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, 
{4, 7, 0}, {5, 1, 0}, {5, 4, 0}, {7, 4, 0}, {1, 1, 2}, {1, 2, 0}, {1, 3, 0}, {2, 5, 0}, {3, 0, 0}, {3, 2, 0}, 
{3, 4, 0}, {4, 1, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {4, 2, 2}, {4, 3, 0}, {7, 5, 0}, {7, 6, 0}, {1, 1, 19}, 
{1, 2, 0}, {1, 3, 0}, {2, 5, 0}, {3, 0, 0}, {3, 2, 0}, {3, 4, 0}, {4, 2, 0}, {4, 3, 0}, {7, 5, 0}, {7, 6, 0},   // +47.967s 47968
{1, 0, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, {3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 1, 0}, {4, 5, 0}, 
{4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, 
{1, 6, 0}, {2, 0, 0}, {2, 1, 0}, {4, 4, 0}, {5, 1, 0}, {5, 4, 0}, {7, 4, 0}, {0, 0, 2}, {5, 3, 0}, {7, 0, 0}, 
{7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 2, 0}, {0, 3, 0}, {5, 2, 0}, {7, 3, 0}, {0, 7, 2}, {2, 6, 0}, {2, 7, 0}, 
{3, 3, 0}, {5, 0, 0}, {6, 0, 0}, {6, 1, 2}, {6, 2, 0}, {6, 3, 0}, {6, 4, 0}, {6, 5, 3}, {6, 6, 0}, {6, 7, 0},   // +48.200s 48208
{6, 7, 20}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {0, 3, 3}, {0, 7, 0}, {2, 6, 0}, {2, 7, 0}, {3, 3, 0}, 
{5, 0, 0}, {6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {5, 2, 0}, {1, 6, 2}, {7, 1, 0}, 
{7, 2, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 1, 0}, 
{4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {7, 0, 0}, {7, 4, 0}, {1, 1, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {3, 1, 0}, 
{3, 5, 0}, {3, 6, 0}, {3, 7, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 7, 0}, {1, 2, 2}, {1, 3, 0},   // +48.767s 48768
{2, 5, 0}, {3, 0, 0}, {3, 2, 0}, {3, 4, 0}, {4, 1, 0}, {4, 2, 0}, {4, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, 
{7, 6, 0}, {1, 7, 19}, {2, 0, 0}, {2, 1, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0}, 
{2, 5, 2}, {2, 6, 0}, {4, 0, 0}, {4, 1, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, 
{0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, 
{0, 7, 2}, {6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {5, 2, 2}, {5, 3, 0},   // +49.300s 49296
{6, 7, 0}, {3, 5, 2}, {3, 6, 0}, {3, 7, 0}, {4, 3, 0}, {5, 1, 0}, {5, 4, 0}, {1, 2, 2}, {1, 3, 0}, {2, 7, 0}, 
{3, 0, 0}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {4, 2, 0}, {1, 0, 2}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, 
{7, 0, 0}, {7, 1, 0}, {1, 6, 3}, {5, 0, 0}, {1, 4, 20}, {1, 5, 0}, {1, 6, 0}, {5, 0, 0}, {7, 0, 2}, {7, 1, 0}, 
{1, 0, 5}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {2, 7, 2}, {3, 0, 0}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, 
{3, 5, 0}, {3, 6, 0}, {4, 2, 0}, {3, 7, 2}, {4, 3, 0}, {5, 1, 0}, {5, 4, 0}, {5, 2, 2}, {5, 3, 0}, {6, 7, 0},   // +49.967s 49968
{6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {0, 2, 2}, {0, 3, 0}, {0, 7, 0}, {6, 0, 0}, {6, 1, 0}, {6, 2, 0}, 
{0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, {4, 0, 2}, {4, 1, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, 
{4, 7, 0}, {2, 3, 2}, {2, 4, 0}, {2, 5, 0}, {2, 6, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, 
{1, 7, 2}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {7, 4, 0}, {7, 2, 2}, {7, 3, 0}, {1, 4, 19}, {1, 5, 0}, {1, 6, 0}, 
{5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {1, 0, 2}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {3, 0, 0}, {2, 7, 2}, {3, 1, 0},   // +50.567s 50560
{3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {3, 5, 0}, {3, 6, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {3, 7, 3}, {5, 2, 0}, 
{5, 3, 0}, {5, 4, 0}, {6, 5, 2}, {6, 6, 0}, {6, 7, 0}, {0, 7, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 0}, {6, 4, 0}, 
{0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {6, 0, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {2, 5, 2}, {2, 6, 0}, 
{4, 0, 0}, {4, 1, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {2, 1, 2}, {2, 2, 0}, {2, 3, 0}, 
{2, 4, 0}, {5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {1, 7, 2}, {2, 0, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0},   // +50.833s 50832
{1, 7, 21}, {2, 0, 0}, {2, 1, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0}, {2, 5, 2}, 
{2, 6, 0}, {4, 0, 0}, {4, 1, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {0, 4, 2}, 
{0, 5, 0}, {0, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {0, 7, 2}, 
{6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {5, 2, 3}, {5, 3, 0}, {6, 7, 0}, 
{3, 5, 2}, {3, 6, 0}, {3, 7, 0}, {4, 3, 0}, {5, 1, 0}, {5, 4, 0}, {1, 2, 2}, {1, 3, 0}, {2, 7, 0}, {3, 0, 0},   // +51.467s 51472
{3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {4, 2, 0}, {1, 0, 2}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {7, 0, 0}, 
{7, 1, 0}, {1, 6, 2}, {5, 0, 0}, {1, 7, 19}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {7, 2, 0}, {7, 3, 0}, {2, 3, 2}, 
{2, 4, 0}, {2, 5, 0}, {2, 6, 0}, {7, 4, 0}, {4, 0, 2}, {4, 1, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, 
{7, 5, 0}, {7, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, 
{0, 2, 2}, {0, 3, 0}, {0, 7, 0}, {6, 0, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0},   // +52.033s 52032
{3, 7, 2}, {5, 2, 0}, {5, 3, 0}, {6, 7, 0}, {2, 7, 2}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {3, 5, 0}, 
{3, 6, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {5, 4, 0}, {1, 0, 2}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {3, 0, 0}, 
{1, 4, 2}, {1, 5, 0}, {1, 6, 0}, {5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {1, 4, 23}, {1, 5, 0}, {1, 6, 0}, {5, 0, 0}, 
{7, 0, 0}, {7, 1, 0}, {1, 0, 2}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {3, 0, 0}, {2, 7, 3}, {3, 1, 0}, {3, 2, 0}, 
{3, 3, 0}, {3, 4, 0}, {3, 5, 0}, {3, 6, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {3, 7, 2}, {5, 2, 0}, {5, 3, 0},   // +52.633s 52640
{5, 4, 0}, {6, 5, 2}, {6, 6, 0}, {6, 7, 0}, {0, 7, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 0}, {6, 4, 0}, {0, 0, 2}, 
{0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {6, 0, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, {2, 5, 2}, {2, 6, 0}, {4, 0, 0}, 
{4, 1, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {4, 7, 0}, {5, 5, 0}, {2, 1, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, 
{5, 6, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {1, 7, 2}, {2, 0, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0}, {1, 6, 19}, 
{5, 0, 0}, {1, 0, 2}, {1, 1, 0}, {1, 4, 0}, {1, 5, 0}, {7, 0, 0}, {7, 1, 0}, {1, 2, 2}, {1, 3, 0}, {2, 7, 0},   // +53.233s 53232
{3, 0, 0}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {4, 2, 0}, {3, 5, 2}, {3, 6, 0}, {3, 7, 0}, {4, 3, 0}, 
{5, 1, 0}, {5, 4, 0}, {5, 2, 2}, {5, 3, 0}, {6, 7, 0}, {6, 3, 2}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {0, 7, 2}, 
{6, 0, 0}, {6, 1, 0}, {6, 2, 0}, {0, 0, 3}, {0, 1, 0}, {0, 2, 0}, {0, 3, 0}, {0, 4, 2}, {0, 5, 0}, {0, 6, 0}, 
{4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {2, 5, 2}, {2, 6, 0}, {4, 0, 0}, {4, 1, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, 
{5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {2, 1, 2}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {7, 4, 0}, {1, 7, 2}, {2, 0, 0},   // +53.533s 53536
{7, 2, 0}, {7, 3, 0}, {1, 7, 21}, {2, 0, 0}, {2, 1, 0}, {2, 2, 0}, {7, 2, 0}, {7, 3, 0}, {2, 3, 2}, {2, 4, 0}, 
{2, 5, 0}, {2, 6, 0}, {7, 4, 0}, {4, 0, 2}, {4, 1, 0}, {4, 7, 0}, {5, 5, 0}, {5, 6, 0}, {5, 7, 0}, {7, 5, 0}, 
{7, 6, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, {0, 5, 0}, {0, 6, 0}, {4, 4, 0}, {4, 5, 0}, {4, 6, 0}, {0, 2, 2}, 
{0, 3, 0}, {0, 7, 0}, {6, 0, 2}, {6, 1, 0}, {6, 2, 0}, {6, 3, 4}, {6, 4, 0}, {6, 5, 0}, {6, 6, 0}, {3, 7, 2}, 
{5, 2, 0}, {5, 3, 0}, {5, 4, 0}, {6, 7, 0}, {2, 7, 2}, {3, 1, 0}, {3, 2, 0}, {3, 3, 0}, {3, 4, 0}, {3, 5, 0},   // +54.167s 54160
{3, 6, 0}, {4, 2, 0}, {4, 3, 0}, {5, 1, 0}, {1, 0, 3}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {3, 0, 0}, {1, 4, 2}, 
{1, 5, 0}, {1, 6, 0}, {5, 0, 0}, {7, 0, 0}, {7, 1, 0}, {0, 4, 20}, {5, 5, 0}, {0, 0, 2}, {0, 1, 0}, {0, 4, 0}, 
{0, 5, 0}, {1, 1, 0}, {1, 4, 0}, {2, 0, 0}, {2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, 
{7, 1, 0}, {0, 0, 3}, {1, 7, 0}, {2, 0, 0}, {2, 1, 0}, {5, 0, 0}, {6, 1, 0}, {0, 6, 2}, {1, 5, 0}, {4, 5, 0}, 
{4, 6, 0}, {5, 0, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 5, 0}, {0, 2, 2}, {0, 5, 0}, {1, 1, 0}, {1, 2, 0},   // +54.700s 54704
{1, 4, 0}, {2, 2, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {5, 2, 0}, {6, 2, 0}, 
{7, 1, 0}, {7, 2, 0}, {7, 4, 0}, {0, 1, 2}, {1, 7, 0}, {2, 1, 0}, {2, 5, 0}, {4, 7, 0}, {5, 7, 0}, {6, 1, 0}, 
{0, 4, 2}, {0, 6, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 7, 0}, {4, 6, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, 
{7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {1, 5, 0}, {2, 2, 0}, {2, 6, 0}, {2, 7, 0}, 
{3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {7, 3, 0}, {0, 5, 2}, {1, 4, 0}, {4, 2, 0},   // +54.833s 54832
{4, 4, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {7, 4, 0}, {0, 0, 2}, {0, 4, 0}, {1, 0, 0}, {1, 6, 0}, 
{2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, {3, 7, 0}, {5, 4, 0}, {7, 0, 0}, 
{7, 6, 0}, {0, 3, 2}, {1, 3, 0}, {2, 7, 0}, {4, 1, 0}, {4, 5, 0}, {5, 6, 0}, {6, 3, 0}, {6, 4, 0}, {7, 3, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, {2, 1, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, 
{4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0},   // +54.967s 54960
{2, 0, 0}, {2, 5, 0}, {6, 5, 0}, {7, 0, 0}, {7, 1, 0}, {0, 4, 3}, {1, 6, 0}, {3, 2, 0}, {3, 6, 0}, {4, 1, 0}, 
{4, 5, 0}, {4, 6, 0}, {5, 1, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 4, 0}, {7, 5, 0}, {0, 2, 2}, {0, 6, 0}, 
{1, 2, 0}, {1, 5, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {5, 3, 0}, 
{7, 2, 0}, {0, 1, 2}, {0, 7, 0}, {1, 1, 0}, {4, 6, 0}, {4, 7, 0}, {5, 1, 0}, {5, 5, 0}, {6, 5, 0}, {6, 6, 0}, 
{7, 1, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {1, 6, 0}, {2, 3, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0},   // +55.100s 55104
{3, 7, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, 
{2, 2, 0}, {2, 4, 0}, {4, 4, 0}, {5, 0, 0}, {5, 6, 0}, {6, 7, 0}, {7, 2, 0}, {7, 3, 0}, {0, 6, 2}, {0, 7, 0}, 
{1, 5, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {6, 6, 0}, {0, 0, 2}, 
{0, 5, 0}, {1, 0, 0}, {1, 4, 0}, {2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {7, 0, 0}, 
{7, 6, 0}, {0, 3, 2}, {1, 3, 0}, {1, 7, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 0, 0}, {5, 4, 0}, {5, 6, 0},   // +55.233s 55232
{5, 7, 0}, {6, 0, 0}, {6, 7, 0}, {7, 3, 0}, {0, 4, 2}, {0, 6, 0}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {3, 1, 0}, 
{3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {5, 3, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 5, 0}, {2, 0, 0}, 
{2, 6, 0}, {3, 0, 0}, {4, 6, 0}, {5, 5, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {7, 1, 0}, {7, 4, 0}, {0, 5, 2}, 
{1, 7, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 5, 0}, {0, 2, 2}, {0, 4, 0}, 
{1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0},   // +55.367s 55360
{6, 2, 0}, {7, 2, 0}, {0, 1, 3}, {1, 1, 0}, {2, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, 
{6, 1, 0}, {7, 1, 0}, {7, 4, 0}, {7, 6, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 3, 0}, 
{3, 6, 0}, {3, 7, 0}, {5, 2, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {1, 4, 0}, 
{2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {4, 1, 0}, {4, 4, 0}, {5, 7, 0}, {6, 2, 0}, {6, 3, 0}, {7, 2, 0}, {0, 4, 2}, 
{2, 7, 0}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0},   // +55.533s 55536
{0, 6, 0}, {1, 0, 0}, {1, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, 
{3, 7, 0}, {5, 1, 0}, {6, 4, 0}, {7, 0, 0}, {7, 3, 0}, {0, 5, 2}, {1, 3, 0}, {4, 1, 0}, {4, 3, 0}, {4, 4, 0}, 
{4, 5, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 3, 0}, {0, 1, 2}, {0, 4, 0}, {1, 1, 0}, {1, 4, 0}, {2, 1, 0}, 
{2, 5, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {0, 7, 0}, {1, 0, 0}, 
{1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {4, 6, 0}, {5, 1, 0}, {5, 6, 0}, {6, 4, 0}, {6, 5, 0}, {7, 0, 0},   // +55.633s 55632
{0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, 
{0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, {2, 2, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, 
{3, 2, 0}, {3, 5, 0}, {5, 0, 0}, {6, 6, 0}, {7, 1, 0}, {7, 2, 0}, {0, 4, 2}, {0, 7, 0}, {4, 6, 0}, {4, 7, 0}, 
{5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 5, 0}, {7, 6, 0}, {0, 3, 2}, {0, 6, 0}, {1, 3, 0}, {1, 5, 0}, {1, 6, 0}, 
{2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 3, 0}, {7, 3, 0}, {7, 5, 0},   // +55.767s 55760
{0, 2, 3}, {1, 2, 0}, {1, 7, 0}, {3, 2, 0}, {4, 4, 0}, {5, 0, 0}, {5, 5, 0}, {6, 6, 0}, {6, 7, 0}, {7, 2, 0}, 
{0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {2, 0, 0}, {2, 5, 0}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 4, 0}, 
{5, 7, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 0, 0}, {1, 3, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 0, 0}, 
{3, 3, 0}, {3, 7, 0}, {6, 0, 0}, {7, 0, 0}, {7, 3, 0}, {7, 4, 0}, {0, 6, 2}, {1, 7, 0}, {3, 5, 0}, {4, 3, 0}, 
{4, 4, 0}, {4, 5, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 7, 0}, {0, 1, 2}, {0, 5, 0}, {1, 1, 0}, {1, 4, 0},   // +55.933s 55936
{1, 5, 0}, {2, 0, 0}, {2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {4, 2, 0}, {5, 2, 0}, 
{7, 1, 0}, {0, 0, 2}, {1, 0, 0}, {2, 7, 0}, {4, 6, 0}, {5, 7, 0}, {6, 0, 0}, {6, 1, 0}, {7, 0, 0}, {7, 4, 0}, 
{0, 4, 2}, {0, 6, 0}, {1, 6, 0}, {2, 2, 0}, {2, 4, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, 
{5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 5, 0}, {2, 1, 0}, 
{2, 6, 0}, {3, 1, 0}, {7, 1, 0}, {7, 2, 0}, {4, 7, 2}, {5, 5, 0}, {0, 3, 2}, {0, 4, 0}, {0, 5, 0}, {1, 3, 0},   // +56.100s 56096
{1, 4, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, 
{4, 6, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 3, 0}, {7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {1, 2, 0}, {2, 7, 0}, 
{4, 1, 0}, {4, 4, 0}, {5, 6, 0}, {6, 1, 0}, {6, 2, 0}, {7, 2, 0}, {0, 6, 2}, {1, 5, 0}, {3, 0, 0}, {3, 4, 0}, 
{4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {1, 3, 0}, {1, 4, 0}, 
{2, 0, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {5, 1, 0}, {6, 3, 0}, {7, 0, 0}, {7, 3, 0},   // +56.200s 56192
{7, 6, 0}, {0, 3, 3}, {4, 1, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 2, 0}, 
{0, 4, 2}, {0, 6, 0}, {1, 1, 0}, {1, 5, 0}, {1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, 
{3, 5, 0}, {4, 2, 0}, {5, 3, 0}, {6, 4, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, {0, 7, 0}, {1, 0, 0}, {2, 0, 0}, 
{2, 6, 0}, {4, 6, 0}, {5, 1, 0}, {5, 5, 0}, {6, 3, 0}, {7, 0, 0}, {0, 5, 2}, {1, 4, 0}, {3, 2, 0}, {3, 6, 0}, 
{4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 5, 0}, {0, 2, 2}, {0, 4, 0}, {1, 1, 0}, {1, 2, 0},   // +56.367s 56368
{1, 6, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {5, 0, 0}, {6, 4, 0}, {6, 5, 0}, 
{7, 1, 0}, {7, 2, 0}, {0, 1, 2}, {0, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {7, 6, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, 
{5, 2, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {1, 7, 0}, {2, 2, 0}, {2, 5, 0}, 
{4, 4, 0}, {5, 0, 0}, {5, 7, 0}, {6, 5, 0}, {6, 6, 0}, {7, 2, 0}, {0, 4, 2}, {1, 6, 0}, {3, 0, 0}, {3, 4, 0},   // +56.500s 56496
{4, 2, 0}, {4, 7, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {0, 6, 0}, {1, 0, 0}, 
{1, 5, 0}, {2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 3, 0}, {3, 7, 0}, {6, 7, 0}, {7, 0, 0}, {7, 3, 0}, 
{7, 4, 0}, {0, 5, 2}, {1, 3, 0}, {1, 7, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, 
{6, 6, 0}, {0, 1, 3}, {0, 4, 0}, {1, 1, 0}, {1, 4, 0}, {1, 6, 0}, {2, 1, 0}, {2, 5, 0}, {3, 0, 0}, {3, 1, 0}, 
{3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {5, 4, 0}, {7, 1, 0}, {0, 0, 2}, {1, 0, 0}, {2, 0, 0}, {2, 4, 0}, {2, 7, 0},   // +56.633s 56640
{4, 6, 0}, {5, 6, 0}, {6, 0, 0}, {6, 7, 0}, {7, 0, 0}, {7, 4, 0}, {0, 5, 2}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, 
{3, 2, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, 
{1, 1, 0}, {1, 2, 0}, {2, 1, 0}, {2, 2, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {6, 1, 0}, {7, 1, 0}, 
{7, 2, 0}, {0, 4, 2}, {1, 6, 0}, {2, 7, 0}, {4, 1, 0}, {4, 6, 0}, {4, 7, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, 
{6, 0, 0}, {7, 6, 0}, {0, 3, 2}, {0, 6, 0}, {1, 3, 0}, {1, 5, 0}, {2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0},   // +56.767s 56768
{3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {5, 3, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {1, 2, 0}, {4, 1, 0}, 
{4, 4, 0}, {5, 1, 0}, {5, 5, 0}, {6, 1, 0}, {6, 2, 0}, {7, 2, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {1, 6, 0}, 
{2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {4, 7, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, 
{0, 0, 2}, {0, 3, 0}, {0, 7, 0}, {1, 0, 0}, {1, 3, 0}, {2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {3, 7, 0}, {6, 3, 0}, 
{7, 0, 0}, {7, 3, 0}, {0, 6, 2}, {1, 5, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 1, 0},   // +56.900s 56896
{5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 2, 0}, {0, 1, 2}, {0, 5, 0}, {1, 1, 0}, {1, 4, 0}, {2, 0, 0}, {2, 1, 0}, 
{2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 2, 0}, {5, 2, 0}, {7, 1, 0}, {0, 0, 2}, {0, 7, 0}, {1, 0, 0}, 
{4, 6, 0}, {5, 0, 0}, {5, 7, 0}, {6, 3, 0}, {6, 4, 0}, {7, 0, 0}, {0, 4, 3}, {0, 6, 0}, {1, 5, 0}, {1, 6, 0}, 
{2, 2, 0}, {2, 4, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {4, 5, 0}, {5, 3, 0}, {5, 4, 0}, 
{5, 6, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 7, 0}, {2, 1, 0}, {2, 6, 0}, {4, 7, 0},   // +57.033s 57040
{6, 5, 0}, {7, 1, 0}, {7, 2, 0}, {0, 5, 2}, {1, 4, 0}, {3, 3, 0}, {3, 7, 0}, {4, 6, 0}, {5, 0, 0}, {5, 2, 0}, 
{5, 5, 0}, {5, 7, 0}, {6, 4, 0}, {7, 6, 0}, {0, 3, 2}, {0, 4, 0}, {1, 3, 0}, {1, 6, 0}, {2, 2, 0}, {2, 3, 0}, 
{2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 6, 0}, {5, 4, 0}, {7, 3, 0}, {7, 5, 0}, {0, 2, 2}, {1, 2, 0}, {1, 7, 0}, 
{4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 5, 0}, {6, 6, 0}, {7, 2, 0}, {7, 4, 0}, 
{0, 5, 2}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, {3, 7, 0},   // +57.167s 57168
{5, 2, 0}, {7, 6, 0}, {0, 0, 2}, {0, 3, 0}, {1, 0, 0}, {1, 3, 0}, {2, 3, 0}, {2, 5, 0}, {2, 7, 0}, {4, 5, 0}, 
{5, 7, 0}, {6, 7, 0}, {7, 0, 0}, {7, 3, 0}, {0, 4, 2}, {1, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, 
{4, 4, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {6, 6, 0}, {7, 4, 0}, {0, 1, 2}, {0, 6, 0}, {1, 1, 0}, {1, 5, 0}, 
{2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {4, 1, 0}, {6, 0, 0}, {7, 1, 0}, {0, 0, 2}, 
{1, 0, 0}, {2, 7, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 7, 0}, {7, 0, 0}, {0, 4, 2},   // +57.333s 57328
{0, 5, 0}, {1, 4, 0}, {1, 6, 0}, {2, 2, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {3, 6, 0}, {4, 3, 0}, 
{5, 4, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {2, 1, 0}, {2, 4, 0}, {4, 7, 0}, {5, 1, 0}, 
{5, 6, 0}, {6, 0, 0}, {6, 1, 0}, {7, 1, 0}, {7, 2, 0}, {0, 6, 3}, {1, 5, 0}, {3, 3, 0}, {3, 7, 0}, {4, 1, 0}, 
{4, 6, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, {7, 6, 0}, {0, 3, 2}, {0, 5, 0}, {1, 3, 0}, {1, 4, 0}, 
{2, 2, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 2, 0}, {3, 6, 0}, {6, 2, 0}, {7, 2, 0}, {7, 3, 0}, {0, 2, 2},   // +57.467s 57472
{0, 7, 0}, {1, 2, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 1, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 1, 0}, 
{0, 0, 2}, {0, 4, 0}, {0, 6, 0}, {1, 5, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, 
{3, 7, 0}, {5, 3, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 2}, {0, 7, 0}, {1, 0, 0}, {1, 3, 0}, {2, 3, 0}, {2, 6, 0}, 
{4, 5, 0}, {5, 0, 0}, {5, 5, 0}, {6, 2, 0}, {6, 3, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {3, 1, 0}, 
{3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {0, 0, 2}, {0, 1, 0}, {1, 1, 0},   // +57.600s 57600
{1, 6, 0}, {1, 7, 0}, {2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, {6, 4, 0}, {7, 0, 0}, 
{7, 1, 0}, {0, 6, 2}, {1, 0, 0}, {1, 5, 0}, {4, 5, 0}, {4, 6, 0}, {5, 0, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, 
{6, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 5, 0}, {1, 2, 0}, {1, 4, 0}, {2, 2, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, 
{3, 5, 0}, {3, 6, 0}, {4, 3, 0}, {5, 2, 0}, {7, 2, 0}, {0, 1, 2}, {1, 1, 0}, {1, 7, 0}, {2, 1, 0}, {2, 5, 0}, 
{4, 7, 0}, {5, 7, 0}, {6, 4, 0}, {6, 5, 0}, {7, 1, 0}, {7, 4, 0}, {0, 4, 2}, {0, 6, 0}, {1, 5, 0}, {1, 6, 0},   // +57.733s 57728
{2, 3, 0}, {2, 4, 0}, {3, 3, 0}, {3, 7, 0}, {4, 6, 0}, {5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, 
{0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 6, 0}, {2, 7, 0}, {3, 2, 0}, {3, 6, 0}, {6, 6, 0}, 
{7, 2, 0}, {7, 3, 0}, {0, 5, 2}, {1, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, 
{6, 5, 0}, {7, 4, 0}, {0, 0, 3}, {0, 4, 0}, {1, 0, 0}, {1, 6, 0}, {2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 5, 0}, 
{3, 0, 0}, {3, 3, 0}, {3, 4, 0}, {3, 7, 0}, {5, 4, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 2}, {1, 3, 0}, {2, 7, 0},   // +57.867s 57872
{4, 1, 0}, {4, 5, 0}, {5, 6, 0}, {6, 6, 0}, {6, 7, 0}, {7, 3, 0}, {0, 5, 2}, {0, 6, 0}, {1, 4, 0}, {1, 5, 0}, 
{2, 1, 0}, {2, 6, 0}, {3, 0, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 2, 0}, 
{5, 3, 0}, {5, 5, 0}, {0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {2, 0, 0}, {2, 5, 0}, {5, 1, 0}, {6, 0, 0}, 
{7, 0, 0}, {7, 1, 0}, {0, 4, 2}, {1, 6, 0}, {3, 2, 0}, {3, 6, 0}, {4, 1, 0}, {4, 6, 0}, {5, 4, 0}, {5, 6, 0}, 
{5, 7, 0}, {6, 7, 0}, {7, 5, 0}, {0, 2, 2}, {0, 6, 0}, {1, 2, 0}, {1, 5, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0},   // +58.000s 58000
{2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 3, 0}, {4, 5, 0}, {5, 3, 0}, {7, 2, 0}, {0, 1, 2}, {0, 7, 0}, {1, 1, 0}, 
{4, 7, 0}, {5, 1, 0}, {5, 5, 0}, {6, 0, 0}, {6, 1, 0}, {7, 1, 0}, {0, 4, 2}, {0, 5, 0}, {1, 4, 0}, {1, 6, 0}, 
{2, 3, 0}, {2, 5, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {5, 2, 0}, {5, 4, 0}, {5, 7, 0}, 
{7, 5, 0}, {7, 6, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 4, 0}, {5, 0, 0}, {5, 6, 0}, 
{6, 2, 0}, {7, 2, 0}, {7, 3, 0}, {0, 6, 2}, {0, 7, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 2, 0},   // +58.133s 58128
{5, 3, 0}, {5, 5, 0}, {6, 1, 0}, {0, 0, 2}, {0, 5, 0}, {1, 0, 0}, {1, 4, 0}, {1, 5, 0}, {2, 0, 0}, {2, 3, 0}, 
{2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {7, 0, 0}, {7, 6, 0}, {0, 3, 3}, {1, 3, 0}, {1, 7, 0}, 
{4, 5, 0}, {5, 0, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 2, 0}, {6, 3, 0}, {7, 3, 0}, {0, 4, 2}, {0, 6, 0}, 
{1, 6, 0}, {2, 1, 0}, {2, 4, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 3, 0}, 
{0, 0, 2}, {0, 1, 0}, {1, 0, 0}, {1, 1, 0}, {1, 5, 0}, {2, 0, 0}, {2, 6, 0}, {3, 0, 0}, {5, 5, 0}, {6, 3, 0},   // +58.267s 58272
{6, 4, 0}, {7, 0, 0}, {7, 1, 0}, {7, 4, 0}, {0, 5, 2}, {1, 7, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, 
{5, 4, 0}, {5, 7, 0}, {7, 5, 0}, {0, 2, 2}, {0, 4, 0}, {1, 2, 0}, {1, 4, 0}, {1, 6, 0}, {2, 1, 0}, {2, 2, 0}, 
{2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {6, 5, 0}, {7, 2, 0}, {0, 1, 2}, {1, 1, 0}, 
{2, 7, 0}, {4, 7, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 4, 0}, {7, 1, 0}, {7, 4, 0}, {7, 6, 0}, {0, 5, 2}, 
{0, 6, 0}, {1, 5, 0}, {2, 3, 0}, {2, 6, 0}, {3, 3, 0}, {3, 6, 0}, {3, 7, 0}, {4, 6, 0}, {5, 2, 0}, {7, 3, 0},   // +58.400s 58400
{7, 5, 0}, {0, 2, 2}, {0, 3, 0}, {1, 2, 0}, {1, 3, 0}, {1, 4, 0}, {2, 2, 0}, {2, 5, 0}, {3, 2, 0}, {4, 1, 0}, 
{5, 7, 0}, {6, 5, 0}, {6, 6, 0}, {7, 2, 0}, {0, 4, 2}, {2, 7, 0}, {3, 4, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, 
{5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 6, 0}, {0, 0, 2}, {0, 6, 0}, {1, 0, 0}, {1, 5, 0}, {1, 6, 0}, {2, 0, 0}, 
{2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {6, 7, 0}, {7, 0, 0}, {7, 3, 0}, {0, 3, 2}, 
{0, 5, 0}, {1, 3, 0}, {4, 1, 0}, {4, 5, 0}, {5, 1, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 6, 0}, {0, 1, 2},   // +58.567s 58560
{0, 4, 0}, {1, 4, 0}, {2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, 
{5, 4, 0}, {7, 1, 0}, {0, 0, 3}, {0, 7, 0}, {1, 0, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0}, {2, 4, 0}, {3, 0, 0}, 
{5, 6, 0}, {6, 0, 0}, {6, 7, 0}, {7, 0, 0}, {0, 5, 2}, {0, 6, 0}, {1, 5, 0}, {3, 6, 0}, {4, 5, 0}, {4, 6, 0}, 
{5, 1, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, {0, 1, 2}, {0, 2, 0}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, 
{2, 1, 0}, {2, 2, 0}, {2, 5, 0}, {2, 6, 0}, {3, 1, 0}, {3, 2, 0}, {3, 5, 0}, {4, 3, 0}, {6, 1, 0}, {7, 1, 0},   // +58.667s 58672
{7, 2, 0}, {0, 4, 2}, {0, 7, 0}, {4, 7, 0}, {5, 0, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 0, 0}, {7, 6, 0}, 
{0, 6, 2}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 7, 0}, {4, 6, 0}, {5, 3, 0}, {7, 5, 0}, {0, 2, 2}, {0, 3, 0}, 
{1, 2, 0}, {1, 3, 0}, {1, 5, 0}, {1, 7, 0}, {2, 2, 0}, {2, 6, 0}, {3, 2, 0}, {3, 3, 0}, {3, 6, 0}, {5, 5, 0}, 
{6, 1, 0}, {6, 2, 0}, {7, 2, 0}, {7, 3, 0}, {0, 4, 2}, {0, 5, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 0, 0}, 
{5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, {0, 0, 2}, {1, 0, 0}, {1, 4, 0}, {1, 6, 0}, {2, 0, 0}, {2, 3, 0},   // +58.833s 58832
{2, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, {3, 7, 0}, {7, 0, 0}, {0, 3, 2}, {0, 6, 0}, {1, 3, 0}, {1, 7, 0}, 
{3, 3, 0}, {4, 5, 0}, {5, 3, 0}, {5, 5, 0}, {5, 6, 0}, {6, 2, 0}, {6, 3, 0}, {7, 3, 0}, {7, 4, 0}, {0, 5, 2}, 
{1, 5, 0}, {3, 1, 0}, {3, 5, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {5, 2, 0}, {0, 1, 2}, {1, 1, 0}, {1, 4, 0}, 
{2, 0, 0}, {2, 1, 0}, {2, 5, 0}, {2, 6, 0}, {2, 7, 0}, {3, 0, 0}, {3, 4, 0}, {6, 4, 0}, {7, 1, 0}, {0, 0, 2}, 
{0, 4, 0}, {0, 6, 0}, {1, 0, 0}, {4, 5, 0}, {4, 6, 0}, {5, 4, 0}, {5, 6, 0}, {5, 7, 0}, {6, 3, 0}, {7, 0, 0},   // +58.967s 58960
{7, 4, 0}, {7, 5, 0}, {3, 2, 2}, {3, 6, 0}, {4, 3, 0}, {5, 3, 0}, {0, 2, 3}, {1, 2, 0}, {1, 5, 0}, {1, 6, 0}, 
{2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 6, 0}, {3, 1, 0}, {3, 5, 0}, {4, 1, 0}, {6, 5, 0}, {7, 2, 0}, {0, 1, 2}, 
{0, 4, 0}, {0, 5, 0}, {1, 1, 0}, {2, 7, 0}, {4, 6, 0}, {4, 7, 0}, {5, 2, 0}, {5, 5, 0}, {5, 7, 0}, {6, 4, 0}, 
{7, 1, 0}, {7, 6, 0}, {3, 3, 2}, {3, 7, 0}, {5, 4, 0}, {7, 5, 0}, {0, 3, 2}, {1, 3, 0}, {1, 4, 0}, {1, 6, 0}, 
{2, 2, 0}, {2, 3, 0}, {2, 4, 0}, {2, 5, 0}, {3, 2, 0}, {3, 6, 0}, {5, 1, 0}, {5, 6, 0}, {6, 6, 0}, {7, 3, 0},   // +59.133s 59136
{0, 2, 2}, {0, 5, 0}, {0, 6, 0}, {1, 2, 0}, {4, 1, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 2, 0}, {5, 3, 0}, 
{5, 5, 0}, {6, 5, 0}, {7, 2, 0}, {1, 5, 2}, {3, 4, 0}, {7, 6, 0}, {0, 0, 2}, {1, 0, 0}, {1, 4, 0}, {2, 0, 0}, 
{2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0}, {5, 7, 0}, {6, 7, 0}, {7, 0, 0}, {0, 3, 2}, 
{0, 4, 0}, {0, 6, 0}, {0, 7, 0}, {1, 3, 0}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, {4, 5, 0}, {5, 1, 0}, {5, 3, 0}, 
{5, 4, 0}, {5, 6, 0}, {6, 6, 0}, {7, 3, 0}, {3, 1, 2}, {3, 5, 0}, {0, 1, 2}, {1, 1, 0}, {1, 5, 0}, {1, 6, 0},   // +59.333s 59328
{2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 6, 0}, {3, 0, 0}, {3, 4, 0}, {5, 0, 0}, {5, 5, 0}, {6, 0, 0}, {7, 1, 0}, 
{0, 0, 2}, {0, 4, 0}, {0, 5, 0}, {0, 7, 0}, {1, 0, 0}, {4, 3, 0}, {4, 5, 0}, {4, 6, 0}, {5, 2, 0}, {5, 4, 0}, 
{5, 7, 0}, {6, 7, 0}, {7, 0, 0}, {7, 5, 0}, {3, 2, 3}, {3, 6, 0}, {0, 2, 2}, {1, 2, 0}, {1, 4, 0}, {1, 6, 0}, 
{1, 7, 0}, {2, 1, 0}, {2, 2, 0}, {2, 4, 0}, {2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {5, 6, 0}, {6, 1, 0}, {7, 1, 0}, 
{7, 2, 0}, {0, 1, 2}, {0, 5, 0}, {0, 6, 0}, {1, 1, 0}, {4, 6, 0}, {4, 7, 0}, {5, 0, 0}, {5, 2, 0}, {5, 3, 0},   // +59.467s 59472
{5, 5, 0}, {6, 0, 0}, {7, 5, 0}, {7, 6, 0}, {1, 5, 2}, {3, 3, 0}, {3, 7, 0}, {0, 2, 2}, {0, 3, 0}, {0, 4, 0}, 
{1, 3, 0}, {1, 4, 0}, {2, 2, 0}, {2, 3, 0}, {2, 5, 0}, {2, 6, 0}, {3, 2, 0}, {3, 6, 0}, {5, 7, 0}, {6, 1, 0}, 
{6, 2, 0}, {7, 2, 0}, {7, 3, 0}, {7, 4, 0}, {0, 6, 2}, {1, 2, 0}, {1, 7, 0}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, 
{5, 3, 0}, {5, 4, 0}, {5, 6, 0}, {7, 6, 0}, {1, 6, 2}, {3, 4, 0}, {0, 0, 2}, {0, 3, 0}, {0, 5, 0}, {1, 0, 0}, 
{1, 3, 0}, {1, 5, 0}, {2, 0, 0}, {2, 3, 0}, {2, 4, 0}, {2, 6, 0}, {2, 7, 0}, {3, 0, 0}, {3, 3, 0}, {3, 7, 0},   // +59.633s 59632
{4, 5, 0}, {5, 5, 0}, {6, 2, 0}, {6, 3, 0}, {7, 0, 0}, {7, 3, 0}, {0, 4, 2}, {4, 2, 0}, {4, 3, 0}, {4, 4, 0}, 
{5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 4, 0}, {1, 4, 2}, {3, 1, 0}, {3, 5, 0}, {7, 1, 0}, {0, 0, 2}, {0, 1, 0}, 
{0, 6, 0}, {1, 0, 0}, {1, 1, 0}, {1, 6, 0}, {2, 0, 0}, {2, 1, 0}, {2, 4, 0}, {2, 5, 0}, {3, 0, 0}, {3, 4, 0}, 
{4, 1, 0}, {5, 6, 0}, {6, 3, 0}, {6, 4, 0}, {7, 0, 0}, {0, 5, 2}, {2, 7, 0}, {4, 3, 0}, {4, 5, 0}, {4, 6, 0}, 
{5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {7, 5, 0}, {1, 5, 3}, {2, 2, 0}, {2, 6, 0}, {3, 2, 0}, {3, 6, 0}, {0, 1, 2},   // +59.833s 59840
{0, 2, 0}, {0, 4, 0}, {1, 1, 0}, {1, 2, 0}, {1, 4, 0}, {2, 1, 0}, {2, 5, 0}, {3, 1, 0}, {3, 5, 0}, {5, 1, 0}, 
{5, 7, 0}, {6, 4, 0}, {6, 5, 0}, {7, 1, 0}, {7, 2, 0}, {0, 6, 2}, {4, 1, 0}, {4, 6, 0}, {4, 7, 0}, {5, 3, 0}, 
{5, 4, 0}, {5, 6, 0}, {7, 5, 0}, {7, 6, 0}, {1, 5, 2}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {3, 2, 0}, {3, 3, 0}, 
{3, 6, 0}, {3, 7, 0}, {0, 2, 2}, {0, 3, 0}, {0, 5, 0}, {0, 7, 0}, {1, 2, 0}, {1, 3, 0}, {2, 2, 0}, {2, 6, 0}, 
{5, 5, 0}, {6, 5, 0}, {6, 6, 0}, {7, 2, 0}, {7, 3, 0}, {0, 4, 2}, {4, 2, 0}, {4, 4, 0}, {4, 7, 0}, {5, 1, 0},   // +59.967s 59968
{5, 2, 0}, {5, 4, 0}, {5, 7, 0}, {7, 6, 0}, {1, 4, 2}, {2, 0, 0}, {2, 5, 0}, {3, 0, 0}, {3, 3, 0}, {3, 4, 0}, 
{3, 7, 0}, {0, 0, 2}, {0, 3, 0}, {0, 6, 0}, {1, 0, 0}, {1, 3, 0}, {1, 6, 0}, {2, 3, 0}, {2, 4, 0}, {4, 3, 0}, 
{4, 5, 0}, {5, 0, 0}, {5, 6, 0}, {6, 6, 0}, {6, 7, 0}, {7, 0, 0}, {7, 3, 0}, {0, 5, 2}, {0, 7, 0}, {4, 2, 0}, 
{4, 4, 0}, {5, 2, 0}, {5, 3, 0}, {5, 5, 0}, {1, 5, 2}, {2, 1, 0}, {2, 6, 0}, {3, 1, 0}, {3, 4, 0}, {3, 5, 0}
};

//------------------------------------------------------------------------------
int numReplays(void)
{
    return sizeof(kLampReplay)/sizeof(kLampReplay[0]);
}

//------------------------------------------------------------------------------
byte replay(byte col)
{
    static byte replayLamps[NUM_COL] = {0};
    static uint32_t lastUpdTtag = 0;
    static uint32_t replayPos = 0;
    //static uint32_t numReplays = (sizeof(kLampReplay) / sizeof(AG_LAMP_SWITCH_t));
    static uint16_t currData = 0;
    AG_LAMP_SWITCH_t *pEv = (AG_LAMP_SWITCH_t*)&currData;
    int nr = numReplays();

    // update the lamp matrix
    uint32_t replayTtag = (PINB & B00000100) ?
        (sTtag >> 6) : //(sTtag / (REPLAY_TTAG_SCALE / TTAG_INT_A));
        (sTtag >> 5);  //(sTtag / (REPLAY_TTAG_SCALE / TTAG_INT_B));
    if (lastUpdTtag == 0)
    {
        lastUpdTtag = replayTtag;
    }
    uint32_t dTtag = (replayTtag - lastUpdTtag);
    if (dTtag >= pEv->dttag)
    {
        // handle all events of this ttag
        currData = pgm_read_word_near(kLampReplay + replayPos);
        do
        {
            replayLamps[pEv->col] ^= (1 << pEv->row);
            replayPos++;
            if (replayPos > nr)
            {
                // start over again
                replayPos = 0;
                lastUpdTtag = 0;
                memset(replayLamps, 0, sizeof(replayLamps));
            }
            currData = pgm_read_word_near(kLampReplay + replayPos);
        } while (pEv->dttag == 0);
        lastUpdTtag = replayTtag;
    }

    // return the current row from the replay lamp matrix
    return replayLamps[col];
}

#endif // REPLAY_ENABLED
