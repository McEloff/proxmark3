//-----------------------------------------------------------------------------
// Jonathan Westhues, Mar 2006
// Edits by Gerhard de Koning Gans, Sep 2007 (##)
//
// This code is licensed to you under the terms of the GNU GPL, version 2 or,
// at your option, any later version. See the LICENSE.txt file for the text of
// the license.
//-----------------------------------------------------------------------------
// The main application code. This is the first thing called after start.c
// executes.
//-----------------------------------------------------------------------------
#include <stdarg.h>
#include <inttypes.h>
#include "usb_cdc.h"
#include "proxmark3.h"
#include "apps.h"
#include "fpga.h"
#include "util.h"
#include "printf.h"
#include "string.h"
#include "legicrf.h"
#include "legicrfsim.h"
#include "lfsampling.h"
#include "BigBuf.h"
#include "mifareutil.h"
#include "mifaresim.h"
#include "hitag.h"

#define DEBUG 1

#ifdef WITH_LCD
#include "LCD.h"
#endif

#ifdef WITH_SMARTCARD
#include "i2c.h"
#endif

#ifdef WITH_FPC
#include "usart.h"
#endif

#ifdef WITH_FLASH
#include "flashmem.h"
#endif

//=============================================================================
// A buffer where we can queue things up to be sent through the FPGA, for
// any purpose (fake tag, as reader, whatever). We go MSB first, since that
// is the order in which they go out on the wire.
//=============================================================================

#define TOSEND_BUFFER_SIZE (9*MAX_FRAME_SIZE + 1 + 1 + 2)  // 8 data bits and 1 parity bit per payload byte, 1 correction bit, 1 SOC bit, 2 EOC bits
uint8_t ToSend[TOSEND_BUFFER_SIZE];
int ToSendMax = -1;
static int ToSendBit;
struct common_area common_area __attribute__((section(".commonarea")));

void ToSendReset(void) {
    ToSendMax = -1;
    ToSendBit = 8;
}

void ToSendStuffBit(int b) {
    if (ToSendBit >= 8) {
        ToSendMax++;
        ToSend[ToSendMax] = 0;
        ToSendBit = 0;
    }

    if (b)
        ToSend[ToSendMax] |= (1 << (7 - ToSendBit));

    ToSendBit++;

    if (ToSendMax >= sizeof(ToSend)) {
        ToSendBit = 0;
        DbpString("ToSendStuffBit overflowed!");
    }
}

/* useful when debugging new protocol implementations like FeliCa
void PrintToSendBuffer(void) {
    DbpString("Printing ToSendBuffer:");
    Dbhexdump(ToSendMax, ToSend, 0);
}
*/

void print_result(char *name, uint8_t *buf, size_t len) {

    uint8_t *p = buf;
    uint16_t tmp = len & 0xFFF0;

    for (; p - buf < tmp; p += 16) {
        Dbprintf("[%s: %02d/%02d] %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                 name,
                 p - buf,
                 len,
                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12], p[13], p[14], p[15]
                );
    }
    if (len % 16 != 0) {
        char s[46] = {0};
        char *sp = s;
        for (; p - buf < len; p++) {
            sprintf(sp, "%02x ", p[0]);
            sp += 3;
        }
        Dbprintf("[%s: %02d/%02d] %s", name, p - buf, len, s);
    }
}

//=============================================================================
// Debug print functions, to go out over USB, to the usual PC-side client.
//=============================================================================

void DbpStringEx(uint32_t flags, char *str) {
#if DEBUG
    struct {
        uint16_t flag;
        uint8_t buf[USB_CMD_DATA_SIZE - sizeof(uint16_t)];
    } PACKED data;
    data.flag = flags;
    uint16_t len = MIN(strlen(str), sizeof(data.buf));
    memcpy(data.buf, str, len);
    reply_ng(CMD_DEBUG_PRINT_STRING, PM3_SUCCESS, (uint8_t *)&data, sizeof(data.flag) + len);
#endif
}

void DbpString(char *str) {
#if DEBUG
    DbpStringEx(FLAG_LOG, str);
#endif
}

#if 0
void DbpIntegers(int x1, int x2, int x3) {
    reply_old(CMD_DEBUG_PRINT_INTEGERS, x1, x2, x3, 0, 0);
}
#endif
void DbprintfEx(uint32_t flags, const char *fmt, ...) {
#if DEBUG
    // should probably limit size here; oh well, let's just use a big buffer
    char output_string[128] = {0x00};
    va_list ap;
    va_start(ap, fmt);
    kvsprintf(fmt, output_string, 10, ap);
    va_end(ap);

    DbpStringEx(flags, output_string);
#endif
}

void Dbprintf(const char *fmt, ...) {
#if DEBUG
    // should probably limit size here; oh well, let's just use a big buffer
    char output_string[128] = {0x00};
    va_list ap;

    va_start(ap, fmt);
    kvsprintf(fmt, output_string, 10, ap);
    va_end(ap);

    DbpString(output_string);
#endif
}

// prints HEX & ASCII
void Dbhexdump(int len, uint8_t *d, bool bAsci) {
#if DEBUG
    char ascii[9];

    while (len > 0) {

        int l = (len > 8) ? 8 : len;

        memcpy(ascii, d, l);
        ascii[l] = 0;

        // filter safe ascii
        for (int i = 0; i < l; i++) {
            if (ascii[i] < 32 || ascii[i] > 126) {
                ascii[i] = '.';
            }
        }

        if (bAsci)
            Dbprintf("%-8s %*D", ascii, l, d, " ");
        else
            Dbprintf("%*D", l, d, " ");

        len -= 8;
        d += 8;
    }
#endif
}

//-----------------------------------------------------------------------------
// Read an ADC channel and block till it completes, then return the result
// in ADC units (0 to 1023). Also a routine to average 32 samples and
// return that.
//-----------------------------------------------------------------------------
static uint16_t ReadAdc(int ch) {

    // Note: ADC_MODE_PRESCALE and ADC_MODE_SAMPLE_HOLD_TIME are set to the maximum allowed value.
    // AMPL_HI is are high impedance (10MOhm || 1MOhm) output, the input capacitance of the ADC is 12pF (typical). This results in a time constant
    // of RC = (0.91MOhm) * 12pF = 10.9us. Even after the maximum configurable sample&hold time of 40us the input capacitor will not be fully charged.
    //
    // The maths are:
    // If there is a voltage v_in at the input, the voltage v_cap at the capacitor (this is what we are measuring) will be
    //
    //       v_cap = v_in * (1 - exp(-SHTIM/RC))  =   v_in * (1 - exp(-40us/10.9us))  =  v_in * 0,97                   (i.e. an error of 3%)

    AT91C_BASE_ADC->ADC_CR = AT91C_ADC_SWRST;
    AT91C_BASE_ADC->ADC_MR =
        ADC_MODE_PRESCALE(63)          // ADC_CLK = MCK / ((63+1) * 2) = 48MHz / 128 = 375kHz
        | ADC_MODE_STARTUP_TIME(1)       // Startup Time = (1+1) * 8 / ADC_CLK = 16 / 375kHz = 42,7us   Note: must be > 20us
        | ADC_MODE_SAMPLE_HOLD_TIME(15); // Sample & Hold Time SHTIM = 15 / ADC_CLK = 15 / 375kHz = 40us

    AT91C_BASE_ADC->ADC_CHER = ADC_CHANNEL(ch);
    AT91C_BASE_ADC->ADC_CR = AT91C_ADC_START;

    while (!(AT91C_BASE_ADC->ADC_SR & ADC_END_OF_CONVERSION(ch))) {};

    return (AT91C_BASE_ADC->ADC_CDR[ch] & 0x3FF);
}

// was static - merlok
uint16_t AvgAdc(int ch) {
    uint16_t a = 0;
    for (uint8_t i = 0; i < 32; i++)
        a += ReadAdc(ch);

    //division by 32
    return (a + 15) >> 5;
}

void MeasureAntennaTuning(void) {

    uint8_t LF_Results[256];
    uint32_t i, peak = 0, peakv = 0, peakf = 0;
    uint32_t v_lf125 = 0, v_lf134 = 0, v_hf = 0; // in mV

    memset(LF_Results, 0, sizeof(LF_Results));
    LED_B_ON();

    /*
     * Sweeps the useful LF range of the proxmark from
     * 46.8kHz (divisor=255) to 600kHz (divisor=19) and
     * read the voltage in the antenna, the result left
     * in the buffer is a graph which should clearly show
     * the resonating frequency of your LF antenna
     * ( hopefully around 95 if it is tuned to 125kHz!)
     */

    FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_LF_ADC | FPGA_LF_ADC_READER_FIELD);
    SpinDelay(50);

    for (i = 255; i >= 19; i--) {
        WDT_HIT();
        FpgaSendCommand(FPGA_CMD_SET_DIVISOR, i);
        SpinDelay(20);
        uint32_t adcval = ((MAX_ADC_LF_VOLTAGE * AvgAdc(ADC_CHAN_LF)) >> 10);
        if (i == 95)
            v_lf125 = adcval; // voltage at 125Khz
        if (i == 89)
            v_lf134 = adcval; // voltage at 134Khz

        LF_Results[i] = adcval >> 9; // scale int to fit in byte for graphing purposes
        if (LF_Results[i] > peak) {
            peakv = adcval;
            peakf = i;
            peak = LF_Results[i];
        }
    }

    LED_A_ON();
    // Let the FPGA drive the high-frequency antenna around 13.56 MHz.
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER_RX_XCORR);
    SpinDelay(50);
    v_hf = (MAX_ADC_HF_VOLTAGE * AvgAdc(ADC_CHAN_HF)) >> 10;

    // RDV40 will hit the roof, try other ADC channel used in that hardware revision.
    if (v_hf > MAX_ADC_HF_VOLTAGE - 300) {
        v_hf = (MAX_ADC_HF_VOLTAGE_RDV40 * AvgAdc(ADC_CHAN_HF_RDV40)) >> 10;
    }

    uint64_t arg0 = v_lf134;
    arg0 <<= 32;
    arg0 |= v_lf125;

    uint64_t arg2 = peakv;
    arg2 <<= 32;
    arg2 |= peakf;

    reply_old(CMD_MEASURED_ANTENNA_TUNING, arg0, v_hf, arg2, LF_Results, 256);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    LEDsoff();
}

void MeasureAntennaTuningHf(void) {
    uint16_t volt = 0; // in mV
    // Let the FPGA drive the high-frequency antenna around 13.56 MHz.
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_HF_READER_RX_XCORR);
    SpinDelay(50);
    volt = (MAX_ADC_HF_VOLTAGE * AvgAdc(ADC_CHAN_HF)) >> 10;
    bool use_high = (volt > MAX_ADC_HF_VOLTAGE - 300);

    while (!BUTTON_PRESS()) {
        SpinDelay(20);
        if (!use_high) {
            volt = (MAX_ADC_HF_VOLTAGE * AvgAdc(ADC_CHAN_HF)) >> 10;
        } else {
            volt = (MAX_ADC_HF_VOLTAGE_RDV40 * AvgAdc(ADC_CHAN_HF_RDV40)) >> 10;
        }
        DbprintfEx(FLAG_INPLACE, "%u mV / %5u V", volt, (uint16_t)(volt / 1000));
    }
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
    DbprintfEx(FLAG_LOG, "\n[+] cancelled", 1);
}

void ReadMem(int addr) {
    const uint8_t *data = ((uint8_t *)addr);

    Dbprintf("%x: %02x %02x %02x %02x %02x %02x %02x %02x", addr, data[0], data[1], data[2], data[3], data[4], data[5], data[6], data[7]);
}

/* osimage version information is linked in */
extern struct version_information version_information;
/* bootrom version information is pointed to from _bootphase1_version_pointer */
extern char *_bootphase1_version_pointer, _flash_start, _flash_end, _bootrom_start, _bootrom_end, __data_src_start__;
void SendVersion(void) {
    char temp[USB_CMD_DATA_SIZE]; /* Limited data payload in USB packets */
    char VersionString[USB_CMD_DATA_SIZE] = { '\0' };

    /* Try to find the bootrom version information. Expect to find a pointer at
     * symbol _bootphase1_version_pointer, perform slight sanity checks on the
     * pointer, then use it.
     */
    char *bootrom_version = *(char **)&_bootphase1_version_pointer;

    strncat(VersionString, " [ ARM ]\n", sizeof(VersionString) - strlen(VersionString) - 1);

    if (bootrom_version < &_flash_start || bootrom_version >= &_flash_end) {
        strcat(VersionString, "bootrom version information appears invalid\n");
    } else {
        FormatVersionInformation(temp, sizeof(temp), " bootrom: ", bootrom_version);
        strncat(VersionString, temp, sizeof(VersionString) - strlen(VersionString) - 1);
    }

    FormatVersionInformation(temp, sizeof(temp), "      os: ", &version_information);
    strncat(VersionString, temp, sizeof(VersionString) - strlen(VersionString) - 1);

    strncat(VersionString, "\n [ FPGA ]\n", sizeof(VersionString) - strlen(VersionString) - 1);

    for (int i = 0; i < fpga_bitstream_num; i++) {
        strncat(VersionString, fpga_version_information[i], sizeof(VersionString) - strlen(VersionString) - 1);
        if (i < fpga_bitstream_num - 1) {
            strncat(VersionString, "\n", sizeof(VersionString) - strlen(VersionString) - 1);
        }
    }
    // Send Chip ID and used flash memory
    uint32_t text_and_rodata_section_size = (uint32_t)&__data_src_start__ - (uint32_t)&_flash_start;
    uint32_t compressed_data_section_size = common_area.arg1;
    reply_old(CMD_ACK, *(AT91C_DBGU_CIDR), text_and_rodata_section_size + compressed_data_section_size, 0, VersionString, strlen(VersionString));
}

// measure the USB Speed by sending SpeedTestBufferSize bytes to client and measuring the elapsed time.
// Note: this mimics GetFromBigbuf(), i.e. we have the overhead of the PacketCommandNG structure included.
void printUSBSpeed(void) {
    DbpStringEx(FLAG_LOG | FLAG_ANSI, _BLUE_("Transfer Speed"));
    Dbprintf("  Sending packets to client...");

#define USB_SPEED_TEST_MIN_TIME 1500 // in milliseconds
    uint8_t *test_data = BigBuf_get_addr();
    uint32_t end_time;

    uint32_t start_time = end_time = GetTickCount();
    uint32_t bytes_transferred = 0;

    LED_B_ON();

    while (end_time < start_time + USB_SPEED_TEST_MIN_TIME) {
        reply_ng(CMD_DOWNLOADED_RAW_ADC_SAMPLES_125K, PM3_SUCCESS, test_data, USB_CMD_DATA_SIZE);
        end_time = GetTickCount();
        bytes_transferred += USB_CMD_DATA_SIZE;
    }
    LED_B_OFF();

    Dbprintf("  Time elapsed............%dms", end_time - start_time);
    Dbprintf("  Bytes transferred.......%d", bytes_transferred);
    DbprintfEx(FLAG_LOG | FLAG_ANSI, "  Transfer Speed PM3 -> Client = " _YELLOW_("%d") " bytes/s", 1000 * bytes_transferred / (end_time - start_time));
}

/**
  * Prints runtime information about the PM3.
**/
void SendStatus(void) {
    BigBuf_print_status();
    Fpga_print_status();
#ifdef WITH_FLASH
    Flashmem_print_status();
#endif
#ifdef WITH_SMARTCARD
    I2C_print_status();
#endif
#ifdef WITH_LF
    printConfig();      // LF Sampling config
    printT55xxConfig(); // LF T55XX Config
#endif
    printUSBSpeed();
    DbpStringEx(FLAG_LOG | FLAG_ANSI, _BLUE_("Various"));
    Dbprintf("  MF_DBGLEVEL.............%d", MF_DBGLEVEL);
    Dbprintf("  ToSendMax...............%d", ToSendMax);
    Dbprintf("  ToSendBit...............%d", ToSendBit);
    Dbprintf("  ToSend BUFFERSIZE.......%d", TOSEND_BUFFER_SIZE);
    DbpStringEx(FLAG_LOG | FLAG_ANSI, _BLUE_("Installed StandAlone Mode"));
    ModInfo();

    //DbpString("Running ");
    //Dbprintf("  Is Device attached to USB| %s", USB_ATTACHED() ? "Yes" : "No");
    //Dbprintf("  Is Device attached to FPC| %s", send_using_0 ? "Yes" : "No");
    //Dbprintf("  Is USB_reconnect value   | %d", GetUSBreconnect() );
    //Dbprintf("  Is USB_configured value  | %d", GetUSBconfigured() );

    reply_old(CMD_ACK, 1, 0, 0, 0, 0);
}

void SendCapabilities(void) {
    capabilities_t capabilities;
    capabilities.via_fpc = reply_via_fpc;
    if (reply_via_fpc)
        capabilities.baudrate = USART_BAUD_RATE;
    else
        capabilities.baudrate = 0; // no real baudrate for USB-CDC
    reply_ng(CMD_CAPABILITIES, PM3_SUCCESS, (uint8_t *)&capabilities, sizeof(capabilities));
}

// Show some leds in a pattern to identify StandAlone mod is running
void StandAloneMode(void) {

    DbpString("Stand-alone mode! No PC necessary.");

    SpinDown(50);
    SpinOff(50);
    SpinUp(50);
    SpinOff(50);
    SpinDown(50);
    SpinDelay(500);
}

/*
OBJECTIVE
Listen and detect an external reader. Determine the best location
for the antenna.

INSTRUCTIONS:
Inside the ListenReaderField() function, there is two mode.
By default, when you call the function, you will enter mode 1.
If you press the PM3 button one time, you will enter mode 2.
If you press the PM3 button a second time, you will exit the function.

DESCRIPTION OF MODE 1:
This mode just listens for an external reader field and lights up green
for HF and/or red for LF. This is the original mode of the detectreader
function.

DESCRIPTION OF MODE 2:
This mode will visually represent, using the LEDs, the actual strength of the
current compared to the maximum current detected. Basically, once you know
what kind of external reader is present, it will help you spot the best location to place
your antenna. You will probably not get some good results if there is a LF and a HF reader
at the same place! :-)
*/
#define LIGHT_LEVELS 20

void ListenReaderField(int limit) {
#define LF_ONLY 1
#define HF_ONLY 2
#define REPORT_CHANGE 10    // report new values only if they have changed at least by REPORT_CHANGE

    uint16_t lf_av = 0, lf_av_new, lf_baseline = 0, lf_max = 0;
    uint16_t hf_av = 0, hf_av_new,  hf_baseline = 0, hf_max = 0;
    uint16_t mode = 1, display_val, display_max;
    bool use_high = false;

    // switch off FPGA - we don't want to measure our own signal
    // 20180315 - iceman,  why load this before and then turn off?
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);

    LEDsoff();

    if (limit == LF_ONLY) {
        lf_av = lf_max = AvgAdc(ADC_CHAN_LF);
        Dbprintf("LF 125/134kHz Baseline: %dmV", (MAX_ADC_LF_VOLTAGE * lf_av) >> 10);
        lf_baseline = lf_av;
    }

    if (limit == HF_ONLY) {

        hf_av = hf_max = AvgAdc(ADC_CHAN_HF);

        // iceman,  useless,  since we are measuring readerfield,  not our field.  My tests shows a max of 20v from a reader.
        // RDV40 will hit the roof, try other ADC channel used in that hardware revision.
        use_high = (((MAX_ADC_HF_VOLTAGE * hf_max) >> 10) > MAX_ADC_HF_VOLTAGE - 300);
        if (use_high) {
            hf_av = hf_max = AvgAdc(ADC_CHAN_HF_RDV40);
        }

        Dbprintf("HF 13.56MHz Baseline: %dmV", (MAX_ADC_HF_VOLTAGE * hf_av) >> 10);
        hf_baseline = hf_av;
    }

    for (;;) {

        // Switch modes with button
        if (BUTTON_PRESS()) {
            SpinDelay(500);
            switch (mode) {
                case 1:
                    mode = 2;
                    DbpString("Signal Strength Mode");
                    break;
                case 2:
                default:
                    DbpString("Stopped");
                    FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
                    LEDsoff();
                    return;
            }
        }
        WDT_HIT();

        if (limit == LF_ONLY) {
            if (mode == 1) {
                if (ABS(lf_av - lf_baseline) > REPORT_CHANGE)
                    LED_D_ON();
                else
                    LED_D_OFF();
            }

            lf_av_new = AvgAdc(ADC_CHAN_LF);
            // see if there's a significant change
            if (ABS(lf_av - lf_av_new) > REPORT_CHANGE) {
                Dbprintf("LF 125/134kHz Field Change: %5dmV", (MAX_ADC_LF_VOLTAGE * lf_av_new) >> 10);
                lf_av = lf_av_new;
                if (lf_av > lf_max)
                    lf_max = lf_av;
            }
        }

        if (limit == HF_ONLY) {
            if (mode == 1) {
                if (ABS(hf_av - hf_baseline) > REPORT_CHANGE)
                    LED_B_ON();
                else
                    LED_B_OFF();
            }

            hf_av_new = (use_high) ? AvgAdc(ADC_CHAN_HF_RDV40) :  AvgAdc(ADC_CHAN_HF);

            // see if there's a significant change
            if (ABS(hf_av - hf_av_new) > REPORT_CHANGE) {
                Dbprintf("HF 13.56MHz Field Change: %5dmV", (MAX_ADC_HF_VOLTAGE * hf_av_new) >> 10);
                hf_av = hf_av_new;
                if (hf_av > hf_max)
                    hf_max = hf_av;
            }
        }

        if (mode == 2) {
            if (limit == LF_ONLY) {
                display_val = lf_av;
                display_max = lf_max;
            } else if (limit == HF_ONLY) {
                display_val = hf_av;
                display_max = hf_max;
            } else { /* Pick one at random */
                if ((hf_max - hf_baseline) > (lf_max - lf_baseline)) {
                    display_val = hf_av;
                    display_max = hf_max;
                } else {
                    display_val = lf_av;
                    display_max = lf_max;
                }
            }

            display_val = display_val * (4 * LIGHT_LEVELS) / MAX(1, display_max);
            uint32_t duty_a = MIN(MAX(display_val, 0 * LIGHT_LEVELS), 1 * LIGHT_LEVELS) - 0 * LIGHT_LEVELS;
            uint32_t duty_b = MIN(MAX(display_val, 1 * LIGHT_LEVELS), 2 * LIGHT_LEVELS) - 1 * LIGHT_LEVELS;
            uint32_t duty_c = MIN(MAX(display_val, 2 * LIGHT_LEVELS), 3 * LIGHT_LEVELS) - 2 * LIGHT_LEVELS;
            uint32_t duty_d = MIN(MAX(display_val, 3 * LIGHT_LEVELS), 4 * LIGHT_LEVELS) - 3 * LIGHT_LEVELS;

            // LED A
            if (duty_a == 0) {
                LED_A_OFF();
            } else if (duty_a == LIGHT_LEVELS) {
                LED_A_ON();
            } else {
                LED_A_ON();
                SpinDelay(duty_a);
                LED_A_OFF();
                SpinDelay(LIGHT_LEVELS - duty_a);
            }

            // LED B
            if (duty_b == 0) {
                LED_B_OFF();
            } else if (duty_b == LIGHT_LEVELS) {
                LED_B_ON();
            } else {
                LED_B_ON();
                SpinDelay(duty_b);
                LED_B_OFF();
                SpinDelay(LIGHT_LEVELS - duty_b);
            }

            // LED C
            if (duty_c == 0) {
                LED_C_OFF();
            } else if (duty_c == LIGHT_LEVELS) {
                LED_C_ON();
            } else {
                LED_C_ON();
                SpinDelay(duty_c);
                LED_C_OFF();
                SpinDelay(LIGHT_LEVELS - duty_c);
            }

            // LED D
            if (duty_d == 0) {
                LED_D_OFF();
            } else if (duty_d == LIGHT_LEVELS) {
                LED_D_ON();
            } else {
                LED_D_ON();
                SpinDelay(duty_d);
                LED_D_OFF();
                SpinDelay(LIGHT_LEVELS - duty_d);
            }
        }
    }
}
static void PacketReceived(PacketCommandNG *packet) {
    /*
    if (packet->ng) {
        Dbprintf("received NG frame with %d bytes payload, with command: 0x%04x", packet->length, cmd);
    } else {
        Dbprintf("received OLD frame of %d bytes, with command: 0x%04x and args: %d %d %d", packet->length, packet->cmd, packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
    }
    */

    switch (packet->cmd) {
#ifdef WITH_LF
        case CMD_SET_LF_T55XX_CONFIG:
            setT55xxConfig(packet->oldarg[0], (t55xx_config *) packet->data.asBytes);
            break;
        case CMD_SET_LF_SAMPLING_CONFIG:
            setSamplingConfig((sample_config *) packet->data.asBytes);
            break;
        case CMD_ACQUIRE_RAW_ADC_SAMPLES_125K: {
            uint32_t bits = SampleLF(packet->oldarg[0], packet->oldarg[1]);
            reply_old(CMD_ACK, bits, 0, 0, 0, 0);
            break;
        }
        case CMD_MOD_THEN_ACQUIRE_RAW_ADC_SAMPLES_125K:
            ModThenAcquireRawAdcSamples125k(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_LF_SNIFF_RAW_ADC_SAMPLES: {
            uint32_t bits = SniffLF();
            reply_old(CMD_ACK, bits, 0, 0, 0, 0);
            break;
        }
        case CMD_HID_DEMOD_FSK: {
            uint32_t high, low;
            CmdHIDdemodFSK(packet->oldarg[0], &high, &low, 1);
            break;
        }
        case CMD_HID_SIM_TAG:
            CmdHIDsimTAG(packet->oldarg[0], packet->oldarg[1], 1);
            break;
        case CMD_FSK_SIM_TAG:
            CmdFSKsimTAG(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes, 1);
            break;
        case CMD_ASK_SIM_TAG:
            CmdASKsimTag(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes, 1);
            break;
        case CMD_PSK_SIM_TAG:
            CmdPSKsimTag(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes, 1);
            break;
        case CMD_HID_CLONE_TAG:
            CopyHIDtoT55x7(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes[0]);
            break;
        case CMD_IO_DEMOD_FSK: {
            uint32_t high, low;
            CmdIOdemodFSK(packet->oldarg[0], &high, &low, 1);
            break;
        }
        case CMD_IO_CLONE_TAG:
            CopyIOtoT55x7(packet->oldarg[0], packet->oldarg[1]);
            break;
        case CMD_EM410X_DEMOD: {
            uint32_t high;
            uint64_t low;
            CmdEM410xdemod(packet->oldarg[0], &high, &low, 1);
            break;
        }
        case CMD_EM410X_WRITE_TAG:
            WriteEM410x(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
            break;
        case CMD_READ_TI_TYPE:
            ReadTItag();
            break;
        case CMD_WRITE_TI_TYPE:
            WriteTItag(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
            break;
        case CMD_SIMULATE_TAG_125K:
            LED_A_ON();
            SimulateTagLowFrequency(packet->oldarg[0], packet->oldarg[1], 1);
            LED_A_OFF();
            break;
        case CMD_LF_SIMULATE_BIDIR:
            SimulateTagLowFrequencyBidir(packet->oldarg[0], packet->oldarg[1]);
            break;
        case CMD_INDALA_CLONE_TAG:
            CopyIndala64toT55x7(packet->data.asDwords[0], packet->data.asDwords[1]);
            break;
        case CMD_INDALA_CLONE_TAG_L:
            CopyIndala224toT55x7(
                packet->data.asDwords[0], packet->data.asDwords[1], packet->data.asDwords[2], packet->data.asDwords[3],
                packet->data.asDwords[4], packet->data.asDwords[5], packet->data.asDwords[6]
            );
            break;
        case CMD_T55XX_READ_BLOCK: {
            T55xxReadBlock(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
            break;
        }
        case CMD_T55XX_WRITE_BLOCK:
            T55xxWriteBlock(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes[0]);
            break;
        case CMD_T55XX_WAKEUP:
            T55xxWakeUp(packet->oldarg[0]);
            break;
        case CMD_T55XX_RESET_READ:
            T55xxResetRead();
            break;
        case CMD_T55XX_CHKPWDS:
            T55xx_ChkPwds();
            break;
        case CMD_PCF7931_READ:
            ReadPCF7931();
            break;
        case CMD_PCF7931_WRITE:
            WritePCF7931(
                packet->data.asBytes[0], packet->data.asBytes[1], packet->data.asBytes[2], packet->data.asBytes[3],
                packet->data.asBytes[4], packet->data.asBytes[5], packet->data.asBytes[6], packet->data.asBytes[9],
                packet->data.asBytes[7] - 128, packet->data.asBytes[8] - 128,
                packet->oldarg[0],
                packet->oldarg[1],
                packet->oldarg[2]
            );
            break;
        case CMD_EM4X_READ_WORD:
            EM4xReadWord(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
            break;
        case CMD_EM4X_WRITE_WORD:
            EM4xWriteWord(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
            break;
        case CMD_AWID_DEMOD_FSK:  {
            uint32_t high, low;
            // Set realtime AWID demodulation
            CmdAWIDdemodFSK(packet->oldarg[0], &high, &low, 1);
            break;
        }
        case CMD_VIKING_CLONE_TAG:
            CopyVikingtoT55xx(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
            break;
        case CMD_COTAG:
            Cotag(packet->oldarg[0]);
            break;
#endif

#ifdef WITH_HITAG
        case CMD_SNIFF_HITAG: // Eavesdrop Hitag tag, args = type
            SniffHitag(packet->oldarg[0]);
            break;
        case CMD_SIMULATE_HITAG: // Simulate Hitag tag, args = memory content
            SimulateHitagTag((bool)packet->oldarg[0], packet->data.asBytes);
            break;
        case CMD_READER_HITAG: // Reader for Hitag tags, args = type and function
            ReaderHitag((hitag_function)packet->oldarg[0], (hitag_data *)packet->data.asBytes);
            break;
        case CMD_SIMULATE_HITAG_S:// Simulate Hitag s tag, args = memory content
            SimulateHitagSTag((bool)packet->oldarg[0], packet->data.asBytes);
            break;
        case CMD_TEST_HITAGS_TRACES:// Tests every challenge within the given file
            check_challenges((bool)packet->oldarg[0], packet->data.asBytes);
            break;
        case CMD_READ_HITAG_S: //Reader for only Hitag S tags, args = key or challenge
            ReadHitagS((hitag_function)packet->oldarg[0], (hitag_data *)packet->data.asBytes);
            break;
        case CMD_WR_HITAG_S: //writer for Hitag tags args=data to write,page and key or challenge
            if ((hitag_function)packet->oldarg[0] < 10) {
                WritePageHitagS((hitag_function)packet->oldarg[0], (hitag_data *)packet->data.asBytes, packet->oldarg[2]);
            } else {
                WriterHitag((hitag_function)packet->oldarg[0], (hitag_data *)packet->data.asBytes, packet->oldarg[2]);
            }
            break;
#endif

#ifdef WITH_ISO15693
        case CMD_ACQUIRE_RAW_ADC_SAMPLES_ISO_15693:
            AcquireRawAdcSamplesIso15693();
            break;
        case CMD_RECORD_RAW_ADC_SAMPLES_ISO_15693:
            RecordRawAdcSamplesIso15693();
            break;
        case CMD_ISO_15693_COMMAND:
            DirectTag15693Command(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_ISO_15693_FIND_AFI:
            BruteforceIso15693Afi(packet->oldarg[0]);
            break;
        case CMD_READER_ISO_15693:
            ReaderIso15693(packet->oldarg[0]);
            break;
        case CMD_SIMTAG_ISO_15693:
            SimTagIso15693(packet->oldarg[0], packet->data.asBytes);
            break;
#endif

#ifdef WITH_LEGICRF
        case CMD_SIMULATE_TAG_LEGIC_RF:
            LegicRfSimulate(packet->oldarg[0]);
            break;
        case CMD_WRITER_LEGIC_RF:
            LegicRfWriter(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_READER_LEGIC_RF:
            LegicRfReader(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
            break;
        case CMD_LEGIC_INFO:
            LegicRfInfo();
            break;
        case CMD_LEGIC_ESET:
            //-----------------------------------------------------------------------------
            // Note: we call FpgaDownloadAndGo(FPGA_BITSTREAM_HF) here although FPGA is not
            // involved in dealing with emulator memory. But if it is called later, it might
            // destroy the Emulator Memory.
            //-----------------------------------------------------------------------------
            // arg0 = offset
            // arg1 = num of bytes
            FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
            emlSet(packet->data.asBytes, packet->oldarg[0], packet->oldarg[1]);
            break;
#endif

#ifdef WITH_ISO14443b
        case CMD_READ_SRI_TAG:
            ReadSTMemoryIso14443b(packet->oldarg[0]);
            break;
        case CMD_SNIFF_ISO_14443B:
            SniffIso14443b();
            break;
        case CMD_SIMULATE_TAG_ISO_14443B:
            SimulateIso14443bTag(packet->oldarg[0]);
            break;
        case CMD_ISO_14443B_COMMAND:
            //SendRawCommand14443B(packet->oldarg[0],packet->oldarg[1],packet->oldarg[2],packet->data.asBytes);
            SendRawCommand14443B_Ex(packet);
            break;
#endif

#ifdef WITH_FELICA
        case CMD_FELICA_COMMAND:
            felica_sendraw(packet);
            break;
        case CMD_FELICA_LITE_SIM:
            felica_sim_lite(packet->oldarg[0]);
            break;
        case CMD_FELICA_SNIFF:
            felica_sniff(packet->oldarg[0], packet->oldarg[1]);
            break;
        case CMD_FELICA_LITE_DUMP:
            felica_dump_lite_s();
            break;
#endif

#ifdef WITH_ISO14443a
        case CMD_SNIFF_ISO_14443a:
            SniffIso14443a(packet->oldarg[0]);
            break;
        case CMD_READER_ISO_14443a:
            ReaderIso14443a(packet);
            break;
        case CMD_SIMULATE_TAG_ISO_14443a:
            SimulateIso14443aTag(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);  // ## Simulate iso14443a tag - pass tag type & UID
            break;
        case CMD_ANTIFUZZ_ISO_14443a:
            iso14443a_antifuzz(packet->oldarg[0]);
            break;
        case CMD_EPA_PACE_COLLECT_NONCE:
            EPA_PACE_Collect_Nonce(packet);
            break;
        case CMD_EPA_PACE_REPLAY:
            EPA_PACE_Replay(packet);
            break;
        case CMD_READER_MIFARE:
            ReaderMifare(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2]);
            break;
        case CMD_MIFARE_READBL:
            MifareReadBlock(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFAREU_READBL:
            MifareUReadBlock(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
        case CMD_MIFAREUC_AUTH:
            MifareUC_Auth(packet->oldarg[0], packet->data.asBytes);
            break;
        case CMD_MIFAREU_READCARD:
            MifareUReadCard(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFAREUC_SETPWD:
            MifareUSetPwd(packet->oldarg[0], packet->data.asBytes);
            break;
        case CMD_MIFARE_READSC:
            MifareReadSector(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFARE_WRITEBL:
            MifareWriteBlock(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        //case CMD_MIFAREU_WRITEBL_COMPAT:
        //MifareUWriteBlockCompat(packet->oldarg[0], packet->data.asBytes);
        //break;
        case CMD_MIFAREU_WRITEBL:
            MifareUWriteBlock(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
        case CMD_MIFARE_ACQUIRE_ENCRYPTED_NONCES:
            MifareAcquireEncryptedNonces(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFARE_ACQUIRE_NONCES:
            MifareAcquireNonces(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFARE_NESTED:
            MifareNested(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFARE_CHKKEYS: {
            MifareChkKeys(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes, packet->ng);
            break;
        }
        case CMD_MIFARE_CHKKEYS_FAST: {
            MifareChkKeys_fast(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        }
        case CMD_SIMULATE_MIFARE_CARD:
            Mifare1ksim(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;

        // emulator
        case CMD_MIFARE_SET_DBGMODE:
            MifareSetDbgLvl(packet->oldarg[0]);
            break;
        case CMD_MIFARE_EML_MEMCLR:
            MifareEMemClr(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFARE_EML_MEMSET:
            MifareEMemSet(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFARE_EML_MEMGET:
            MifareEMemGet(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFARE_EML_CARDLOAD:
            MifareECardLoad(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;

        // Work with "magic Chinese" card
        case CMD_MIFARE_CSETBLOCK:
            MifareCSetBlock(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
        case CMD_MIFARE_CGETBLOCK:
            MifareCGetBlock(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
        case CMD_MIFARE_CIDENT:
            MifareCIdent();
            break;
        // mifare sniffer
//        case CMD_MIFARE_SNIFFER:
//            SniffMifare(packet->oldarg[0]);
//            break;
        case CMD_MIFARE_SETMOD:
            MifareSetMod(packet->oldarg[0], packet->data.asBytes);
            break;
        //mifare desfire
        case CMD_MIFARE_DESFIRE_READBL:
            break;
        case CMD_MIFARE_DESFIRE_WRITEBL:
            break;
        case CMD_MIFARE_DESFIRE_AUTH1:
            MifareDES_Auth1(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_MIFARE_DESFIRE_AUTH2:
            //MifareDES_Auth2(packet->oldarg[0],packet->data.asBytes);
            break;
        case CMD_MIFARE_DES_READER:
            //readermifaredes(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
        case CMD_MIFARE_DESFIRE_INFO:
            MifareDesfireGetInformation();
            break;
        case CMD_MIFARE_DESFIRE:
            MifareSendCommand(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
        case CMD_MIFARE_COLLECT_NONCES:
            break;
        case CMD_MIFARE_NACK_DETECT:
            DetectNACKbug();
            break;
#endif

#ifdef WITH_ICLASS
        // Makes use of ISO14443a FPGA Firmware
        case CMD_SNIFF_ICLASS:
            SniffIClass();
            break;
        case CMD_SIMULATE_TAG_ICLASS:
            SimulateIClass(packet->oldarg[0], packet->oldarg[1], packet->oldarg[2], packet->data.asBytes);
            break;
        case CMD_READER_ICLASS:
            ReaderIClass(packet->oldarg[0]);
            break;
        case CMD_READER_ICLASS_REPLAY:
            ReaderIClass_Replay(packet->oldarg[0], packet->data.asBytes);
            break;
        case CMD_ICLASS_EML_MEMSET:
            //iceman, should call FPGADOWNLOAD before, since it corrupts BigBuf
            FpgaDownloadAndGo(FPGA_BITSTREAM_HF);
            emlSet(packet->data.asBytes, packet->oldarg[0], packet->oldarg[1]);
            break;
        case CMD_ICLASS_WRITEBLOCK:
            iClass_WriteBlock(packet->oldarg[0], packet->data.asBytes);
            break;
        case CMD_ICLASS_READCHECK:  // auth step 1
            iClass_ReadCheck(packet->oldarg[0], packet->oldarg[1]);
            break;
        case CMD_ICLASS_READBLOCK:
            iClass_ReadBlk(packet->oldarg[0]);
            break;
        case CMD_ICLASS_AUTHENTICATION: //check
            iClass_Authentication(packet->data.asBytes);
            break;
        case CMD_ICLASS_CHECK_KEYS:
            iClass_Authentication_fast(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
        case CMD_ICLASS_DUMP:
            iClass_Dump(packet->oldarg[0], packet->oldarg[1]);
            break;
        case CMD_ICLASS_CLONE:
            iClass_Clone(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
#endif

#ifdef WITH_HFSNIFF
        case CMD_HF_SNIFFER:
            HfSniff(packet->oldarg[0], packet->oldarg[1]);
            break;
#endif

#ifdef WITH_SMARTCARD
        case CMD_SMART_ATR: {
            SmartCardAtr();
            break;
        }
        case CMD_SMART_SETBAUD: {
            SmartCardSetBaud(packet->oldarg[0]);
            break;
        }
        case CMD_SMART_SETCLOCK: {
            SmartCardSetClock(packet->oldarg[0]);
            break;
        }
        case CMD_SMART_RAW: {
            SmartCardRaw(packet->oldarg[0], packet->oldarg[1], packet->data.asBytes);
            break;
        }
        case CMD_SMART_UPLOAD: {
            // upload file from client
            uint8_t *mem = BigBuf_get_addr();
            memcpy(mem + packet->oldarg[0], packet->data.asBytes, USB_CMD_DATA_SIZE);
            reply_old(CMD_ACK, 1, 0, 0, 0, 0);
            break;
        }
        case CMD_SMART_UPGRADE: {
            SmartCardUpgrade(packet->oldarg[0]);
            break;
        }
#endif

#ifdef WITH_FPC
        case CMD_FPC_SEND: {


            StartTicks();
            DbpString("Mutual USB/FPC sending from device to client");

            /*
            char at[11] = {'\0'};
            static const char* s_at = "AT+BAUD8\0D\0A";
            strncat(at, s_at, sizeof(at) - strlen(at) - 1);
            DbpString("Try AT baud rate setting");
            usart_init();
            int16_t res = usart_writebuffer_sync((uint8_t*)&at, sizeof(at));
            WaitMS(1);
            Dbprintf("SEND %d | %c%c%c%c%c%c%c%c%c%c%c", res,  at[0], at[1], at[2], at[3], at[4], at[5], at[6], at[7], at[8], at[9], at[10]);

            uint8_t my_rx[20];
            memset(my_rx, 0, sizeof(my_rx));
            res = usart_readbuffer(my_rx);
            WaitMS(1);
            Dbprintf("GOT  %d | %c%c%c%c%c%c%c%c", res,  my_rx[0], my_rx[1], my_rx[2], my_rx[3], my_rx[4], my_rx[5], my_rx[6], my_rx[7]);
            */


            char dest[USART_FIFOLEN] = {'\0'};
            uint16_t available = usart_rxdata_available();
            if (available > 0) {
                Dbprintf("RX DATA!");
                uint16_t len = usart_read_ng((uint8_t *)dest, available);
                dest[len] = '\0';
                Dbprintf("RX: %d | %02X %02X %02X %02X %02X %02X %02X %02X ", len,  dest[0], dest[1], dest[2], dest[3], dest[4], dest[5], dest[6], dest[7]);
            }

            static const char *welcome = "Proxmark3 Serial interface via FPC ready\r\n";
            usart_writebuffer_sync((uint8_t *)welcome, strlen(welcome));

            sprintf(dest, "| bytes 0x%02x 0x%02x 0x%02x 0x%02x\r\n"
                    , packet->data.asBytes[0]
                    , packet->data.asBytes[1]
                    , packet->data.asBytes[2]
                    , packet->data.asBytes[3]
                   );
            usart_writebuffer_sync((uint8_t *)dest, strlen(dest));


            LED_A_ON();


            //usb
            reply_old(CMD_DEBUG_PRINT_STRING, strlen(dest), 0, 0, dest, strlen(dest));
            LED_A_OFF();
            /*
            uint8_t my_rx[sizeof(PacketCommandOLD)];
            while (!BUTTON_PRESS() && !usb_poll_validate_length()) {
                LED_B_INV();
                if (usart_read_ng(my_rx) ) {
                    //PacketReceived(my_rx, sizeof(my_rx));

                    PacketCommandOLD *my = (PacketCommandOLD *)my_rx;
                    if (my->cmd > 0 ) {
                        Dbprintf("received command: 0x%04x and args: %d %d %d", my->cmd, my->arg[0], my->arg[1], my->arg[2]);
                    }
                }
            }
            */
            //reply_old(CMD_DEBUG_PRINT_STRING, strlen(dest), 0, 0, dest, strlen(dest));

            reply_old(CMD_ACK, 0, 0, 0, 0, 0);
            StopTicks();
            break;
        }
#endif
        case CMD_BUFF_CLEAR:
            BigBuf_Clear();
            BigBuf_free();
            break;

        case CMD_MEASURE_ANTENNA_TUNING:
            MeasureAntennaTuning();
            break;

        case CMD_MEASURE_ANTENNA_TUNING_HF:
            MeasureAntennaTuningHf();
            break;

        case CMD_LISTEN_READER_FIELD:
            ListenReaderField(packet->oldarg[0]);
            break;

        case CMD_FPGA_MAJOR_MODE_OFF: // ## FPGA Control
            FpgaWriteConfWord(FPGA_MAJOR_MODE_OFF);
            SpinDelay(200);
            LED_D_OFF(); // LED D indicates field ON or OFF
            break;
#ifdef WITH_LF
        case CMD_DOWNLOAD_RAW_ADC_SAMPLES_125K: {
            LED_B_ON();
            uint8_t *mem = BigBuf_get_addr();
            uint32_t startidx = packet->oldarg[0];
            uint32_t numofbytes = packet->oldarg[1];
            // arg0 = startindex
            // arg1 = length bytes to transfer
            // arg2 = BigBuf tracelen
            //Dbprintf("transfer to client parameters: %" PRIu32 " | %" PRIu32 " | %" PRIu32, startidx, numofbytes, packet->oldarg[2]);

            for (size_t i = 0; i < numofbytes; i += USB_CMD_DATA_SIZE) {
                size_t len = MIN((numofbytes - i), USB_CMD_DATA_SIZE);
                int result = reply_old(CMD_DOWNLOADED_RAW_ADC_SAMPLES_125K, i, len, BigBuf_get_traceLen(), mem + startidx + i, len);
                if (result != PM3_SUCCESS)
                    Dbprintf("transfer to client failed ::  | bytes between %d - %d (%d) | result: %d", i, i + len, len, result);
            }
            // Trigger a finish downloading signal with an ACK frame
            // iceman,  when did sending samplingconfig array got attached here?!?
            // arg0 = status of download transfer
            // arg1 = RFU
            // arg2 = tracelen?
            // asbytes = samplingconfig array
            reply_old(CMD_ACK, 1, 0, BigBuf_get_traceLen(), getSamplingConfig(), sizeof(sample_config));
            LED_B_OFF();
            break;
        }
#endif
        case CMD_UPLOAD_SIM_SAMPLES_125K: {
            // iceman; since changing fpga_bitstreams clears bigbuff, Its better to call it before.
            // to be able to use this one for uploading data to device
            // arg1 = 0 upload for LF usage
            //        1 upload for HF usage
#define FPGA_LF 1
            if (packet->oldarg[1] == FPGA_LF)
                FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
            else
                FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

            uint8_t *mem = BigBuf_get_addr();
            memcpy(mem + packet->oldarg[0], packet->data.asBytes, USB_CMD_DATA_SIZE);
            reply_old(CMD_ACK, 1, 0, 0, 0, 0);
            break;
        }
        case CMD_DOWNLOAD_EML_BIGBUF: {
            LED_B_ON();
            uint8_t *mem = BigBuf_get_EM_addr();
            size_t len = 0;
            uint32_t startidx = packet->oldarg[0];
            uint32_t numofbytes = packet->oldarg[1];

            // arg0 = startindex
            // arg1 = length bytes to transfer
            // arg2 = RFU

            for (size_t i = 0; i < numofbytes; i += USB_CMD_DATA_SIZE) {
                len = MIN((numofbytes - i), USB_CMD_DATA_SIZE);
                int result = reply_old(CMD_DOWNLOADED_EML_BIGBUF, i, len, 0, mem + startidx + i, len);
                if (result != PM3_SUCCESS)
                    Dbprintf("transfer to client failed ::  | bytes between %d - %d (%d) | result: %d", i, i + len, len, result);
            }
            // Trigger a finish downloading signal with an ACK frame
            reply_old(CMD_ACK, 1, 0, 0, 0, 0);
            LED_B_OFF();
            break;
        }
        case CMD_READ_MEM:
            ReadMem(packet->oldarg[0]);
            break;
#ifdef WITH_FLASH
        case CMD_FLASHMEM_SET_SPIBAUDRATE:
            FlashmemSetSpiBaudrate(packet->oldarg[0]);
            break;
        case CMD_FLASHMEM_READ: {
            LED_B_ON();
            uint32_t startidx = packet->oldarg[0];
            uint16_t len = packet->oldarg[1];

            Dbprintf("FlashMem read | %d - %d | ", startidx, len);

            size_t size = MIN(USB_CMD_DATA_SIZE, len);

            if (!FlashInit()) {
                break;
            }

            uint8_t *mem = BigBuf_malloc(size);

            for (size_t i = 0; i < len; i += size) {
                len = MIN((len - i), size);

                Dbprintf("FlashMem reading  | %d | %d | %d |", startidx + i, i, len);
                uint16_t isok = Flash_ReadDataCont(startidx + i, mem, len);
                if (isok == len) {
                    print_result("Chunk: ", mem, len);
                } else {
                    Dbprintf("FlashMem reading failed | %d | %d", len, isok);
                    break;
                }
            }
            BigBuf_free();
            FlashStop();
            LED_B_OFF();
            break;
        }
        case CMD_FLASHMEM_WRITE: {
            LED_B_ON();
            uint8_t isok = 0;
            uint16_t res = 0;
            uint32_t startidx = packet->oldarg[0];
            uint16_t len = packet->oldarg[1];
            uint8_t *data = packet->data.asBytes;

            uint32_t tmp = startidx + len;

            if (!FlashInit()) {
                break;
            }

            Flash_CheckBusy(BUSY_TIMEOUT);
            Flash_WriteEnable();

            if (startidx == DEFAULT_T55XX_KEYS_OFFSET)
                Flash_Erase4k(3, 0xC);
            else if (startidx ==  DEFAULT_MF_KEYS_OFFSET)
                Flash_Erase4k(3, 0xB);
            else if (startidx == DEFAULT_ICLASS_KEYS_OFFSET)
                Flash_Erase4k(3, 0xA);

            Flash_CheckBusy(BUSY_TIMEOUT);
            Flash_WriteEnable();

            // inside 256b page?
            if ((tmp & 0xFF) != 0) {

                // is offset+len larger than a page
                tmp = (startidx & 0xFF) + len;
                if (tmp > 0xFF) {

                    // data spread over two pages.

                    // offset xxxx10,
                    uint8_t first_len = (~startidx & 0xFF) + 1;

                    // first mem page
                    res = Flash_WriteDataCont(startidx, data, first_len);

                    isok = (res == first_len) ? 1 : 0;

                    // second mem page
                    res = Flash_WriteDataCont(startidx + first_len, data + first_len, len - first_len);

                    isok &= (res == (len - first_len)) ? 1 : 0;

                } else {
                    res = Flash_WriteDataCont(startidx, data, len);
                    isok = (res == len) ? 1 : 0;
                }
            } else {
                res = Flash_WriteDataCont(startidx, data, len);
                isok = (res == len) ? 1 : 0;
            }
            FlashStop();

            reply_old(CMD_ACK, isok, 0, 0, 0, 0);
            LED_B_OFF();
            break;
        }
        case CMD_FLASHMEM_WIPE: {
            LED_B_ON();
            uint8_t page = packet->oldarg[0];
            uint8_t initalwipe = packet->oldarg[1];
            bool isok = false;
            if (initalwipe) {
                isok = Flash_WipeMemory();
                reply_old(CMD_ACK, isok, 0, 0, 0, 0);
                LED_B_OFF();
                break;
            }
            if (page < 3)
                isok = Flash_WipeMemoryPage(page);

            reply_old(CMD_ACK, isok, 0, 0, 0, 0);
            LED_B_OFF();
            break;
        }
        case CMD_FLASHMEM_DOWNLOAD: {

            LED_B_ON();
            uint8_t *mem = BigBuf_malloc(USB_CMD_DATA_SIZE);
            uint32_t startidx = packet->oldarg[0];
            uint32_t numofbytes = packet->oldarg[1];
            // arg0 = startindex
            // arg1 = length bytes to transfer
            // arg2 = RFU

            if (!FlashInit()) {
                break;
            }

            for (size_t i = 0; i < numofbytes; i += USB_CMD_DATA_SIZE) {
                size_t len = MIN((numofbytes - i), USB_CMD_DATA_SIZE);

                bool isok = Flash_ReadDataCont(startidx + i, mem, len);
                if (!isok)
                    Dbprintf("reading flash memory failed ::  | bytes between %d - %d", i, len);

                isok = reply_old(CMD_FLASHMEM_DOWNLOADED, i, len, 0, mem, len);
                if (isok != 0)
                    Dbprintf("transfer to client failed ::  | bytes between %d - %d", i, len);
            }
            FlashStop();

            reply_old(CMD_ACK, 1, 0, 0, 0, 0);
            BigBuf_free();
            LED_B_OFF();
            break;
        }
        case CMD_FLASHMEM_INFO: {

            LED_B_ON();
            rdv40_validation_t *info = (rdv40_validation_t *)BigBuf_malloc(sizeof(rdv40_validation_t));

            bool isok = Flash_ReadData(FLASH_MEM_SIGNATURE_OFFSET, info->signature, FLASH_MEM_SIGNATURE_LEN);

            if (FlashInit()) {
                Flash_UniqueID(info->flashid);
                FlashStop();
            }
            reply_old(CMD_ACK, isok, 0, 0, info, sizeof(rdv40_validation_t));
            BigBuf_free();

            LED_B_OFF();
            break;
        }
#endif
        case CMD_SET_LF_DIVISOR:
            FpgaDownloadAndGo(FPGA_BITSTREAM_LF);
            FpgaSendCommand(FPGA_CMD_SET_DIVISOR, packet->oldarg[0]);
            break;

        case CMD_SET_ADC_MUX:
            switch (packet->oldarg[0]) {
                case 0:
                    SetAdcMuxFor(GPIO_MUXSEL_LOPKD);
                    break;
                case 2:
                    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);
                    break;
#ifndef WITH_FPC
                case 1:
                    SetAdcMuxFor(GPIO_MUXSEL_LORAW);
                    break;
                case 3:
                    SetAdcMuxFor(GPIO_MUXSEL_HIRAW);
                    break;
#endif
            }
            break;

        case CMD_VERSION:
            SendVersion();
            break;
        case CMD_STATUS:
            SendStatus();
            break;
        case CMD_CAPABILITIES:
            SendCapabilities();
        case CMD_PING:
            if (packet->ng) {
                reply_ng(CMD_PING, PM3_SUCCESS, packet->data.asBytes, packet->length);
            } else {
                reply_mix(CMD_ACK, reply_via_fpc, 0, 0, 0, 0);
            }
            break;
#ifdef WITH_LCD
        case CMD_LCD_RESET:
            LCDReset();
            break;
        case CMD_LCD:
            LCDSend(packet->oldarg[0]);
            break;
#endif
        case CMD_SETUP_WRITE:
        case CMD_FINISH_WRITE:
        case CMD_HARDWARE_RESET:
            usb_disable();

            // (iceman) why this wait?
            SpinDelay(1000);
            AT91C_BASE_RSTC->RSTC_RCR = RST_CONTROL_KEY | AT91C_RSTC_PROCRST;
            // We're going to reset, and the bootrom will take control.
            for (;;) {}
            break;

        case CMD_START_FLASH:
            if (common_area.flags.bootrom_present) {
                common_area.command = COMMON_AREA_COMMAND_ENTER_FLASH_MODE;
            }
            usb_disable();
            AT91C_BASE_RSTC->RSTC_RCR = RST_CONTROL_KEY | AT91C_RSTC_PROCRST;
            // We're going to flash, and the bootrom will take control.
            for (;;) {}
            break;

        case CMD_DEVICE_INFO: {
            uint32_t dev_info = DEVICE_INFO_FLAG_OSIMAGE_PRESENT | DEVICE_INFO_FLAG_CURRENT_MODE_OS;
            if (common_area.flags.bootrom_present) {
                dev_info |= DEVICE_INFO_FLAG_BOOTROM_PRESENT;
            }
            reply_old(CMD_DEVICE_INFO, dev_info, 0, 0, 0, 0);
            break;
        }
        default:
            Dbprintf("%s: 0x%04x", "unknown command:", packet->cmd);
            break;
    }
}

void  __attribute__((noreturn)) AppMain(void) {

    SpinDelay(100);
    clear_trace();

    if (common_area.magic != COMMON_AREA_MAGIC || common_area.version != 1) {
        /* Initialize common area */
        memset(&common_area, 0, sizeof(common_area));
        common_area.magic = COMMON_AREA_MAGIC;
        common_area.version = 1;
    }
    common_area.flags.osimage_present = 1;

    LEDsoff();

    // The FPGA gets its clock from us from PCK0 output, so set that up.
    AT91C_BASE_PIOA->PIO_BSR = GPIO_PCK0;
    AT91C_BASE_PIOA->PIO_PDR = GPIO_PCK0;
    AT91C_BASE_PMC->PMC_SCER |= AT91C_PMC_PCK0;
    // PCK0 is PLL clock / 4 = 96Mhz / 4 = 24Mhz
    AT91C_BASE_PMC->PMC_PCKR[0] = AT91C_PMC_CSS_PLL_CLK | AT91C_PMC_PRES_CLK_4; //  4 for 24Mhz pck0, 2 for 48 MHZ pck0
    AT91C_BASE_PIOA->PIO_OER = GPIO_PCK0;

    // Reset SPI
    AT91C_BASE_SPI->SPI_CR = AT91C_SPI_SWRST;
    AT91C_BASE_SPI->SPI_CR = AT91C_SPI_SWRST; // errata says it needs twice to be correctly set.

    // Reset SSC
    AT91C_BASE_SSC->SSC_CR = AT91C_SSC_SWRST;

    // Configure MUX
    SetAdcMuxFor(GPIO_MUXSEL_HIPKD);

    // Load the FPGA image, which we have stored in our flash.
    // (the HF version by default)
    FpgaDownloadAndGo(FPGA_BITSTREAM_HF);

    StartTickCount();

#ifdef WITH_LCD
    LCDInit();
#endif

#ifdef WITH_SMARTCARD
    I2C_init();
#endif

#ifdef WITH_FPC
    usart_init();
#endif

#ifdef WITH_FLASH
    loadT55xxConfig();
#endif

    // This is made as late as possible to ensure enumeration without timeout
    // against device such as http://www.hobbytronics.co.uk/usb-host-board-v2
    usb_disable();
    usb_enable();

    for (;;) {
        WDT_HIT();

        // Check if there is a packet available
        PacketCommandNG rx;
        int ret = receive_ng(&rx);
        if (ret == PM3_SUCCESS) {
            PacketReceived(&rx);
        } else if (ret != PM3_ENODATA) {
            Dbprintf("Error in frame reception: %d", ret);
            // TODO if error, shall we resync ?
        }

        // Press button for one second to enter a possible standalone mode
        if (BUTTON_HELD(1000) > 0) {

            /*
            * So this is the trigger to execute a standalone mod.  Generic entrypoint by following the standalone/standalone.h headerfile
            * All standalone mod "main loop" should be the RunMod() function.
            */
            RunMod();
        }
    }
}
