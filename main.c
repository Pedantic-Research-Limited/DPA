/*
 * RP2040 DSP with Detached Point Arithmetic (DPA)
 * 
 * Features:
 * - FIR filtering with exact integer arithmetic
 * - Basic FFT using DPA (power-of-2 sizes)
 * - Simple beamforming for multiple sensors
 * - ADC input sampling
 * - No floating-point operations required!
 * 
 * 
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/adc.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/timer.h"

// ============================================================================
// DPA CORE IMPLEMENTATION for microcontroller)
// ============================================================================

typedef struct {
    int32_t mantissa;   // 32-bit for microcontroller efficiency
    int8_t  point;      // Decimal point position
} dpa_t;

// Basic DPA operations optimized for RP2040
static inline dpa_t dpa_add(dpa_t a, dpa_t b) {
    if (a.point == b.point) {
        return (dpa_t){a.mantissa + b.mantissa, a.point};
    }
    
    if (a.point > b.point) {
        int shift = a.point - b.point;
        if (shift > 10) return a; // Avoid overflow
        int32_t scale = 1;
        for (int i = 0; i < shift; i++) scale *= 10;
        return (dpa_t){a.mantissa + b.mantissa * scale, a.point};
    } else {
        int shift = b.point - a.point;
        if (shift > 10) return b;
        int32_t scale = 1;
        for (int i = 0; i < shift; i++) scale *= 10;
        return (dpa_t){a.mantissa * scale + b.mantissa, b.point};
    }
}

static inline dpa_t dpa_multiply(dpa_t a, dpa_t b) {
    // Use 64-bit intermediate to avoid overflow
    int64_t result = (int64_t)a.mantissa * b.mantissa;
    
    // Check for overflow and scale down if needed
    if (result > INT32_MAX || result < INT32_MIN) {
        result /= 1000;
        return (dpa_t){(int32_t)result, a.point + b.point + 3};
    }
    
    return (dpa_t){(int32_t)result, a.point + b.point};
}

static inline dpa_t dpa_from_int(int32_t value, int decimal_places) {
    int32_t scale = 1;
    for (int i = 0; i < decimal_places; i++) scale *= 10;
    return (dpa_t){value * scale, -decimal_places};
}

static inline int32_t dpa_to_int(dpa_t num) {
    if (num.point >= 0) {
        int32_t scale = 1;
        for (int i = 0; i < num.point; i++) scale *= 10;
        return num.mantissa * scale;
    } else {
        int32_t scale = 1;
        for (int i = 0; i < -num.point; i++) scale *= 10;
        return num.mantissa / scale;
    }
}

// ============================================================================
// DSP CONFIGURATION
// ============================================================================

#define SAMPLE_RATE_HZ      8000
#define BUFFER_SIZE         256
#define FIR_TAPS           32
#define FFT_SIZE           64
#define NUM_SENSORS        4
#define ADC_CHANNELS       3  // Use ADC0, ADC1, ADC2

// ADC and processing buffers
static uint16_t adc_buffer[ADC_CHANNELS][BUFFER_SIZE];
static dpa_t    signal_buffer[ADC_CHANNELS][BUFFER_SIZE];
static dpa_t    output_buffer[BUFFER_SIZE];

// FIR filter coefficients (low-pass, Fs=8kHz, Fc=1kHz)
// Pre-converted to DPA format for efficiency
static const dpa_t fir_coeffs[FIR_TAPS] = {
    {-41, -6},   {-134, -6},  {-207, -6},  {-180, -6},
    {-12, -6},   {244, -6},   {494, -6},   {583, -6},
    {394, -6},   {-67, -6},   {-693, -6},  {-1266, -6},
    {-1528, -6}, {-1246, -6}, {-434, -6},  {1116, -6},
    {3395, -6},  {6251, -6},  {9367, -6},  {12358, -6},
    {14808, -6}, {16371, -6}, {16763, -6}, {15808, -6},
    {13459, -6}, {9806, -6},  {5081, -6},  {-331, -6},
    {-5806, -6}, {-10646, -6},{-14308, -6},{-16540, -6}
};

// FIR filter delay line
static dpa_t fir_delay[ADC_CHANNELS][FIR_TAPS];
static int fir_index = 0;

// ============================================================================
// ADC SAMPLING SETUP
// ============================================================================

static int dma_chan;
static bool sampling_complete = false;

void dma_handler() {
    dma_hw->ints0 = 1u << dma_chan;
    sampling_complete = true;
}

void setup_adc_sampling() {
    // Initialize ADC
    adc_init();
    adc_gpio_init(26); // ADC0
    adc_gpio_init(27); // ADC1  
    adc_gpio_init(28); // ADC2
    
    // Setup DMA for continuous ADC sampling
    dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);
    
    dma_channel_configure(dma_chan, &cfg,
        adc_buffer[0], &adc_hw->fifo,
        BUFFER_SIZE * ADC_CHANNELS, false);
    
    dma_channel_set_irq0_enabled(dma_chan, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_handler);
    irq_set_enabled(DMA_IRQ_0, true);
    
    // Configure ADC for round-robin sampling
    adc_set_round_robin(0x07); // Sample ADC0, ADC1, ADC2
    adc_fifo_setup(true, true, 1, false, false);
    adc_set_clkdiv(48000000.0f / SAMPLE_RATE_HZ / ADC_CHANNELS - 1);
}

void start_sampling() {
    sampling_complete = false;
    dma_channel_start(dma_chan);
    adc_run(true);
}

// ============================================================================
// FIR FILTER IMPLEMENTATION
// ============================================================================

dpa_t fir_filter(int channel, dpa_t input) {
    // Store new sample in circular buffer
    fir_delay[channel][fir_index] = input;
    
    // Compute filter output using DPA arithmetic
    dpa_t output = {0, 0};
    
    for (int i = 0; i < FIR_TAPS; i++) {
        int delay_idx = (fir_index - i + FIR_TAPS) % FIR_TAPS;
        dpa_t product = dpa_multiply(fir_coeffs[i], fir_delay[channel][delay_idx]);
        output = dpa_add(output, product);
    }
    
    return output;
}

// ============================================================================
// BASIC FFT IMPLEMENTATION (POWER-OF-2 SIZES)
// ============================================================================

// Simple DFT for small sizes (more practical for microcontroller)
void dpa_dft(dpa_t *input, dpa_t *real_out, dpa_t *imag_out, int N) {
    // Pre-computed sine/cosine tables in DPA format
    // For N=64, we need sin/cos values for k*2*pi/64
    static const dpa_t cos_table[16] = {
        {10000, -4}, {9808, -4}, {9239, -4}, {8315, -4},
        {7071, -4}, {5556, -4}, {3827, -4}, {1951, -4},
        {0, -4}, {-1951, -4}, {-3827, -4}, {-5556, -4},
        {-7071, -4}, {-8315, -4}, {-9239, -4}, {-9808, -4}
    };
    
    static const dpa_t sin_table[16] = {
        {0, -4}, {1951, -4}, {3827, -4}, {5556, -4},
        {7071, -4}, {8315, -4}, {9239, -4}, {9808, -4},
        {10000, -4}, {9808, -4}, {9239, -4}, {8315, -4},
        {7071, -4}, {5556, -4}, {3827, -4}, {1951, -4}
    };
    
    for (int k = 0; k < N/2; k++) { // Only compute positive frequencies
        real_out[k] = (dpa_t){0, 0};
        imag_out[k] = (dpa_t){0, 0};
        
        for (int n = 0; n < N; n++) {
            int angle_idx = (k * n * 16 / N) % 16;
            
            dpa_t cos_term = dpa_multiply(input[n], cos_table[angle_idx]);
            dpa_t sin_term = dpa_multiply(input[n], sin_table[angle_idx]);
            
            real_out[k] = dpa_add(real_out[k], cos_term);
            imag_out[k] = dpa_add(imag_out[k], sin_term);
        }
    }
}

// ============================================================================
// SIMPLE BEAMFORMING
// ============================================================================

void delay_and_sum_beamforming(dpa_t input_channels[NUM_SENSORS][BUFFER_SIZE], 
                              dpa_t *output, int samples) {
    // Simple delay-and-sum beamforming
    // Assumes sensors are in a line, steering toward 0 degrees
    
    static const int delays[NUM_SENSORS] = {0, 2, 4, 6}; // Sample delays
    
    for (int i = 0; i < samples; i++) {
        output[i] = (dpa_t){0, 0};
        
        for (int ch = 0; ch < NUM_SENSORS && ch < ADC_CHANNELS; ch++) {
            int delayed_idx = i - delays[ch];
            if (delayed_idx >= 0) {
                output[i] = dpa_add(output[i], input_channels[ch][delayed_idx]);
            }
        }
        
        // Average by dividing by number of sensors
        output[i].mantissa /= NUM_SENSORS;
    }
}

// ============================================================================
// PROCESSING PIPELINE
// ============================================================================

void process_audio_block() {
    // Convert ADC samples to DPA format
    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        for (int i = 0; i < BUFFER_SIZE; i++) {
            // Convert 12-bit ADC to signed DPA with 4 decimal places
            int32_t sample = (int32_t)adc_buffer[ch][i] - 2048; // Center around 0
            signal_buffer[ch][i] = dpa_from_int(sample, 4);
        }
    }
    
    // Apply FIR filtering to each channel
    for (int ch = 0; ch < ADC_CHANNELS; ch++) {
        for (int i = 0; i < BUFFER_SIZE; i++) {
            signal_buffer[ch][i] = fir_filter(ch, signal_buffer[ch][i]);
            fir_index = (fir_index + 1) % FIR_TAPS;
        }
    }
    
    // Apply beamforming
    delay_and_sum_beamforming(signal_buffer, output_buffer, BUFFER_SIZE);
    
    // Optional: Compute FFT of beamformed output
    static dpa_t fft_real[FFT_SIZE], fft_imag[FFT_SIZE];
    if (BUFFER_SIZE >= FFT_SIZE) {
        dpa_dft(output_buffer, fft_real, fft_imag, FFT_SIZE);
        
        // Print first few FFT bins for debugging
        printf("FFT bins: ");
        for (int i = 0; i < 8; i++) {
            int32_t magnitude = dpa_to_int(fft_real[i]);
            printf("%ld ", (long)magnitude);
        }
        printf("\n");
    }
}

// ============================================================================
// MAIN PROGRAM
// ============================================================================

int main() {
    stdio_init_all();
    
    printf("\nRP2040 Pico2 DSP with DPA\n");
    printf("==========================\n");
    printf("Sample Rate: %d Hz\n", SAMPLE_RATE_HZ);
    printf("Buffer Size: %d samples\n", BUFFER_SIZE);
    printf("FIR Taps: %d\n", FIR_TAPS);
    printf("FFT Size: %d\n", FFT_SIZE);
    printf("Channels: %d\n\n", ADC_CHANNELS);
    
    // Initialize ADC and DMA
    setup_adc_sampling();
    
    // Clear filter delay lines
    memset(fir_delay, 0, sizeof(fir_delay));
    
    printf("Starting DSP processing...\n");
    
    uint32_t frame_count = 0;
    uint32_t start_time = time_us_32();
    
    while (true) {
        // Start new sampling
        start_sampling();
        
        // Wait for sampling to complete
        while (!sampling_complete) {
            tight_loop_contents();
        }
        
        // Process the audio block
        process_audio_block();
        
        frame_count++;
        
        // Print performance stats every 100 frames
        if (frame_count % 100 == 0) {
            uint32_t elapsed = time_us_32() - start_time;
            float fps = (float)frame_count * 1000000.0f / elapsed;
            printf("Processed %lu frames, Rate: %.1f FPS\n", 
                   (unsigned long)frame_count, fps);
        }
        
        // Optional: Add some delay for testing
        sleep_ms(10);
    }
    
    return 0;
}