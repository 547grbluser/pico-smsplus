/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/divider.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "hardware/i2c.h"
#include "hardware/interp.h"
#include "hardware/timer.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include <hardware/sync.h>
#include <pico/multicore.h>
#include <hardware/flash.h>
#include <memory>
#include <math.h>
#include <dvi/dvi.h>
#include <util/dump_bin.h>
#include <util/exclusive_proc.h>
#include <util/work_meter.h>
#include <tusb.h>

#include "shared.h"
#include "mytypes.h"
#include "gamepad.h"
#ifdef __cplusplus

#include "ff.h"

#endif
const uint LED_PIN = PICO_DEFAULT_LED_PIN;

#ifndef DVICONFIG
// #define DVICONFIG dviConfig_PicoDVI
// #define DVICONFIG dviConfig_PicoDVISock
#define DVICONFIG dviConfig_PimoroniDemoDVSock
#endif

#define ERRORMESSAGESIZE 40
#define GAMESAVEDIR "/SAVES"
util::ExclusiveProc exclProc_;
char *ErrorMessage;
bool isFatalError = false;
static FATFS fs;
char *romName;

static bool fps_enabled = false;
static uint32_t start_tick_us = 0;
static uint32_t fps = 0;
static int audio_buffer[SMS_AUD_RATE / SMS_FPS];

namespace
{
    constexpr uint32_t CPUFreqKHz = 252000;

    constexpr dvi::Config dviConfig_PicoDVI = {
        .pinTMDS = {10, 12, 14},
        .pinClock = 8,
        .invert = true,
    };
    // Breadboard with Adafruit compononents
    constexpr dvi::Config dviConfig_PicoDVISock = {
        .pinTMDS = {12, 18, 16},
        .pinClock = 14,
        .invert = false,
    };
    // Pimoroni Digital Video, SD Card & Audio Demo Board
    constexpr dvi::Config dviConfig_PimoroniDemoDVSock = {
        .pinTMDS = {8, 10, 12},
        .pinClock = 6,
        .invert = true,
    };
    // Adafruit Feather RP2040 DVI
    constexpr dvi::Config dviConfig_AdafruitFeatherDVI = {
        .pinTMDS = {18, 20, 22},
        .pinClock = 16,
        .invert = true,
    };

    std::unique_ptr<dvi::DVI> dvi_;

    enum class ScreenMode
    {
        SCANLINE_8_7,
        NOSCANLINE_8_7,
        SCANLINE_1_1,
        NOSCANLINE_1_1,
        MAX,
    };
    ScreenMode screenMode_{};

    bool scaleMode8_7_ = true;

    void applyScreenMode()
    {
        bool scanLine = false;

        switch (screenMode_)
        {
        case ScreenMode::SCANLINE_1_1:
            scaleMode8_7_ = false;
            scanLine = true;
            break;

        case ScreenMode::SCANLINE_8_7:
            scaleMode8_7_ = true;
            scanLine = true;
            break;

        case ScreenMode::NOSCANLINE_1_1:
            scaleMode8_7_ = false;
            scanLine = false;
            break;

        case ScreenMode::NOSCANLINE_8_7:
            scaleMode8_7_ = true;
            scanLine = false;
            break;
        }

        dvi_->setScanLine(scanLine);
    }
    void screenMode(int incr)
    {
        screenMode_ = static_cast<ScreenMode>((static_cast<int>(screenMode_) + incr) & 3);
        applyScreenMode();
    }
}

namespace
{
    dvi::DVI::LineBuffer *currentLineBuffer_{};
}

void __not_in_flash_func(drawWorkMeterUnit)(int timing,
                                            [[maybe_unused]] int span,
                                            uint32_t tag)
{
    if (timing >= 0 && timing < 640)
    {
        auto p = currentLineBuffer_->data();
        p[timing] = tag; // tag = color
    }
}

void __not_in_flash_func(drawWorkMeter)(int line)
{
    if (!currentLineBuffer_)
    {
        return;
    }

    memset(currentLineBuffer_->data(), 0, 64);
    memset(&currentLineBuffer_->data()[320 - 32], 0, 64);
    (*currentLineBuffer_)[160] = 0;
    if (line == 4)
    {
        for (int i = 1; i < 10; ++i)
        {
            (*currentLineBuffer_)[16 * i] = 31;
        }
    }

    constexpr uint32_t clocksPerLine = 800 * 10;
    constexpr uint32_t meterScale = 160 * 65536 / (clocksPerLine * 2);
    util::WorkMeterEnum(meterScale, 1, drawWorkMeterUnit);
}

extern "C" void in_ram(sms_palette_sync)(int index)
{
    int r, g, b;
    // Calculate the correct rgb color values
    // The R, G and B values are binary 01 10 11 00 shifted 6 bits to the left
    // So 01 = 01000000 = 40, 10 = 10000000 = 128, 11 = 11000000 = 192, 00 = 00000000 = 0
    // See https://segaretro.org/Palette
    switch (bitmap.pal.color[index][0])
    {
    case 40:
        r = 85;
        break;
    case 128:
        r = 170;
        break;
    case 192:
        r = 255;
        break;
    default:
        r = 0;
    }
    switch (bitmap.pal.color[index][1])
    {
    case 40:
        g = 85;
        break;
    case 128:
        g = 170;
        break;
    case 192:
        g = 255;
        break;
    default:
        g = 0;
    }
    switch (bitmap.pal.color[index][2])
    {
    case 40:
        b = 85;
        break;
    case 128:
        b = 170;
        break;
    case 192:
        b = 255;
        break;
    default:
        b = 0;
    }
    // Store the RGB565 value in the palette
    palette565[index] = MAKE_PIXEL(r, g, b);
}
#define SCANLINEOFFSET 25
extern "C" void in_ram(sms_render_line)(int line, const uint8_t *buffer)
{
    // screen line 0 - 3 do not use
    // screen Line 4 - 235 are the visible screen
    // SMS renders 192 lines

    line += SCANLINEOFFSET;
    if (line < 4)
        return;
    if (line == SCANLINEOFFSET)
    {
        // insert blank lines on top
        for (int bl = 4; bl < SCANLINEOFFSET; bl++)
        {
            auto blank = dvi_->getLineBuffer();
            uint16_t *sbuffer = blank->data() + 32;
            __builtin_memset(sbuffer, 0, 512);
            dvi_->setLineBuffer(bl, blank);
        }
    }

    auto b = dvi_->getLineBuffer();
    uint16_t *sbuffer = b->data() + 32;
    for (int i = screenCropX; i < BMP_WIDTH - screenCropX; i++)
    {
        sbuffer[i - screenCropX] = palette565[(buffer[i + BMP_X_OFFSET]) & 31];
    }
    dvi_->setLineBuffer(line, b);
    if (line == (SMS_HEIGHT + SCANLINEOFFSET - 1))
    {
        // insert blank lines on bottom
        for (int bl = line + 1; bl < 236; bl++)
        {
            auto blank = dvi_->getLineBuffer();
            uint16_t *sbuffer = blank->data() + 32;
            __builtin_memset(sbuffer, 0, 512);
            dvi_->setLineBuffer(bl, blank);
        }
    }
}

void system_load_sram(void)
{
    printf("system_load_sram: TODO\n");

    // TODO
}

void system_save_sram()
{
    printf("system_save_sram: saving sram TODO\n");

    // TODO
}

void system_load_state()
{
    // TODO
}

void system_save_state()
{
    // TODO
}

void in_ram(core1_main)()
{
    printf("core1 started\n");
    while (true)
    {
        dvi_->registerIRQThisCore();
        dvi_->waitForValidLine();

        dvi_->start();
        while (!exclProc_.isExist())
        {
            if (scaleMode8_7_)
            {
                dvi_->convertScanBuffer12bppScaled16_7(34, 32, 288 * 2);
                // 34 + 252 + 34
                // 32 + 576 + 32
            }
            else
            {
                dvi_->convertScanBuffer12bpp();
            }
        }

        dvi_->unregisterIRQThisCore();
        dvi_->stop();

        exclProc_.processOrWaitIfExist();
    }
}
static DWORD prevButtons[2]{};
static DWORD prevButtonssystem[2]{};
static int rapidFireMask[2]{};
static int rapidFireCounter = 0;
void processinput()
{

    int smssystem[2]{};
    for (int i = 0; i < 2; i++)
    {
        auto &gp = io::getCurrentGamePadState(i);
        int smsbuttons = (gp.buttons & io::GamePadState::Button::LEFT ? INPUT_LEFT : 0) |
                         (gp.buttons & io::GamePadState::Button::RIGHT ? INPUT_RIGHT : 0) |
                         (gp.buttons & io::GamePadState::Button::UP ? INPUT_UP : 0) |
                         (gp.buttons & io::GamePadState::Button::DOWN ? INPUT_DOWN : 0) |
                         (gp.buttons & io::GamePadState::Button::A ? INPUT_BUTTON1 : 0) |
                         (gp.buttons & io::GamePadState::Button::B ? INPUT_BUTTON2 : 0) | 0;

        smssystem[i] =
            (gp.buttons & io::GamePadState::Button::SELECT ? INPUT_PAUSE : 0) |
            (gp.buttons & io::GamePadState::Button::START ? INPUT_START : 0) |
            0;

        input.pad[i] = smsbuttons;
        auto p1 = smssystem[i];

        auto pushed = smsbuttons & ~prevButtons[i];
        auto pushedsystem = smssystem[i] & ~prevButtonssystem[i];

        if (p1 & INPUT_START)
        {
            // Toggle frame rate display
            if (pushed & INPUT_BUTTON1)
            {
                fps_enabled = !fps_enabled;
                printf("FPS: %s\n", fps_enabled ? "ON" : "OFF");
            }
            if (pushed & INPUT_UP)
            {
                screenMode(-1);
            }
            else if (pushed & INPUT_DOWN)
            {
                screenMode(+1);
            }
        }
        prevButtons[i] = smsbuttons;
        prevButtonssystem[i] = smssystem[i];
    }
    input.system = smssystem[0] | smssystem[1];
}

// int InfoNES_GetSoundBufferSize() {
//     //printf("InfoNES_GetSoundBufferSize\r\n");
//     return AUDIO_SAMPLES * sizeof(uint8_t);
// }

// void InfoNES_SoundOutput(int samples, BYTE *w1, BYTE *w2, BYTE *w3, BYTE *w4, BYTE *w5) {
//     //printf("InfoNES_SoundOutput: samples = %i\r\n", samples);
//     uint8_t byte;

//     for (uint_fast32_t i = 0; i < samples; i++) {
//         byte = (w1[i] + w2[i] + w3[i] + w4[i] + w5[i]) / 5;
//         audio_buffer[audio_buffer_index] = (int16_t) ((byte << 8) - 32768); // U8 > S16
//         audio_buffer_index++;
//         if (audio_buffer_index == AUDIO_SAMPLES) {
//             s_core->getAudio()->play(audio_buffer, AUDIO_SAMPLES);
//             audio_buffer_index = 0;
//         }
//     }
// }

//   uint16_t m_rate = 44100;
        // uint16_t m_samples = 735;
        // uint8_t m_channels = 2;
        // uint8_t m_volume = 50;
        // uint8_t m_volume_max = 80;
// void in_ram(PicoAudio::play)(const void *data, int samples) {
//     auto buffer = take_audio_buffer(p_producer_pool, true);
//     auto sampleBuffer = (int16_t *) buffer->buffer->bytes;
//     if (m_volume == 100) {
//         memcpy(sampleBuffer, data, samples * sizeof(int16_t) * m_channels);
//     } else {
//         auto dataBuffer = (int16_t *) data;
//         for (uint_fast16_t i = 0; i < (uint_fast16_t) samples * m_channels; i++) {
//             sampleBuffer[i] = (int16_t) ((dataBuffer[i] * m_volume) / 100);
//         }
//     }

//     buffer->sample_count = samples;
//     give_audio_buffer(p_producer_pool, buffer);
// }

void in_ram(processaudio)(void)
{
    uint8_t m_channels = 2;
    uint8_t m_volume = 50;
    uint8_t m_volume_max = 80;
    for (int x = 0; x < snd.bufsize; x++) {
       audio_buffer[x] = (snd.buffer[0][x] << 16) + snd.buffer[1][x];
       //audio_buffer[x] = ((snd.buffer[0][x] + snd.buffer[1][x]) / 512) + 128;
    }

    short *wave = (short *)audio_buffer;
    // ?  int soundbuffersize = dvi_->getAudioRingBuffer().getFullWritableSize();
    int samples = snd.bufsize * m_channels;;
    sizeof(short);
    sizeof(int);  
    sizeof(BYTE);
    int i = 0;
    while (samples) {
       
        auto &ring = dvi_->getAudioRingBuffer();
        auto n = std::min<int>(samples, ring.getWritableSize());
        if (!n)
        {
            return;
        }
        auto p = ring.getWritePointer();

        int ct = n;
        
        while (ct--)
        {
            short l = (int16_t) ((*wave++ * m_volume) / 100);
            short r = l;
            *p++ = {static_cast<short>(l), static_cast<short>(r)};
        }
        ring.advanceWritePointer(n);
        samples -= n;
    }    
    //getAudio()->play((void *) &audio_buffer, snd.bufsize);
   
}
void in_ram(process)(void)
{
    uint32_t frame = 0;
    while (true)
    {
        processinput();
        sms_frame(0);
        processaudio();
        gpio_put(LED_PIN, hw_divider_s32_quotient_inlined(dvi_->getFrameCounter(), 60) & 1);
        tuh_task();
    }
}

/// @brief
/// Start emulator. Emulator does not run well in DEBUG mode, lots of red screen flicker. In order to keep it running fast enough, we need to run it in release mode or in
/// RelWithDebugInfo mode.
/// @return
int main()
{
    // Set voltage and clock frequency
    vreg_set_voltage(VREG_VOLTAGE_1_20);
    sleep_ms(10);
    set_sys_clock_khz(CPUFreqKHz, true);

    stdio_init_all();
    for (int i = 0; i < 2; i++)
    {
        printf("Hello, world! The master system emulator is starting...(%d)\n", i);
        sleep_ms(500);
    }

    printf("Starting Master System Emulator\n");
    gpio_init(LED_PIN);
    gpio_set_dir(LED_PIN, GPIO_OUT);
    gpio_put(LED_PIN, 1);

    // usb initialise
    printf("USB Initialising\n");
    tusb_init();
    //
    printf("Initialising DVI\n");
    dvi_ = std::make_unique<dvi::DVI>(pio0, &DVICONFIG,
                                      dvi::getTiming640x480p60Hz());
    //    dvi_->setAudioFreq(48000, 25200, 6144);
    dvi_->setAudioFreq(44100, 28000, 6272);
    dvi_->allocateAudioBuffer(256);
    //    dvi_->setExclusiveProc(&exclProc_);

    dvi_->getBlankSettings().top = 4 * 2;
    dvi_->getBlankSettings().bottom = 4 * 2;
    // dvi_->setScanLine(true);

    applyScreenMode();

    // 空サンプル詰めとく
    dvi_->getAudioRingBuffer().advanceWritePointer(255);

    multicore_launch_core1(core1_main);
    // smsp_gamedata_set(argv[1]);
    // Check the type of ROM
    // sms.console = strcmp(strrchr(argv[1], '.'), ".gg") ? CONSOLE_SMS : CONSOLE_GG;
    // sms.console = CONSOLE_SMS; // For now, we only support SMS
    //
    printf("Loading ROM\n");
    load_rom();
    // Initialize all systems and power on
    system_init(SMS_AUD_RATE);
    // load state if any
    // system_load_state();
    system_reset();
    printf("Starting game\n");
    process();
    return 0;
}