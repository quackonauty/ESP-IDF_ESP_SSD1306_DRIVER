#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <driver/i2c_master.h>
#include <esp_err.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/* =========================================================================
 * Compile-time constants — derived from Kconfig
 *
 * Only values that are genuinely derived (not simple aliases) are defined
 * here. Raw Kconfig values (CONFIG_ESP_SSD1306_DRIVER_WIDTH, etc.) are used
 * directly in the implementation.
 * ====================================================================== */

#define ESP_SSD1306_DRIVER_TOTAL_PAGES (CONFIG_ESP_SSD1306_DRIVER_HEIGHT / 8)
#define ESP_SSD1306_DRIVER_BUF_SIZE (CONFIG_ESP_SSD1306_DRIVER_WIDTH * ESP_SSD1306_DRIVER_TOTAL_PAGES)
#define ESP_SSD1306_DRIVER_TIMEOUT_MS 1000
#define ESP_SSD1306_DRIVER_PRINTF_BUF_SIZE (((CONFIG_ESP_SSD1306_DRIVER_WIDTH / 8u) * 2u) + 1u)

/* =========================================================================
 * Handle
 * ====================================================================== */

/**
 * @brief Runtime handle for the I2C SSD1306 display.
 *
 * Geometry (width, height, total_pages) and I2C parameters are compile-time
 * constants derived from Kconfig, so the struct only carries state that
 * genuinely varies at runtime:
 *
 *  - The I2C device handle.
 *  - A FreeRTOS mutex that serialises I2C bus transactions.
 *  - A dirty-page bitmask: tracks which pages have been modified since the
 *    last flush. Declared _Atomic (C11) so all read-modify-write operations
 *    on it are atomic without mixing volatile semantics and GCC __atomic
 *    builtins.
 *  - The pixel buffer itself, sized exactly for the configured display.
 *
 * @note
 *  **Thread safety model:**
 *  The internal mutex serialises *I2C transfers* only. Buffer-write
 *  functions (buffer_fill, buffer_text, …) are fast RAM operations and are
 *  **not** individually mutex-protected. If multiple tasks draw and flush
 *  concurrently, protect the full draw-then-flush sequence with an
 *  application-level mutex.
 *
 * @note Initialise with i2c_ssd1306_init(); do not write fields directly.
 */
typedef struct
{
    i2c_master_dev_handle_t i2c_master_dev;
    SemaphoreHandle_t mutex;
    _Atomic uint8_t dirty_pages;
    uint8_t buffer[ESP_SSD1306_DRIVER_BUF_SIZE];
} i2c_ssd1306_handle_t;

/* =========================================================================
 * Lifecycle
 * ====================================================================== */

/**
 * @brief Initialise the SSD1306 display.
 *
 * All display geometry and I2C parameters are taken from Kconfig constants;
 * no configuration struct is required. On success the display is on, the
 * pixel buffer is zeroed, and the dirty-page mask is cleared.
 *
 * On any failure the function cleans up all resources it allocated before
 * returning, so the handle is safe to pass to i2c_ssd1306_deinit() even
 * after an error.
 *
 * @param bus    Initialised I2C master bus handle.
 * @param handle Pointer to an uninitialised handle that will be filled in.
 *
 * @return
 *   - ESP_OK              Success.
 *   - ESP_ERR_NO_MEM      Mutex creation failed (out of heap).
 *   - ESP_ERR_NOT_FOUND   No device responded at CONFIG_ESP_SSD1306_DRIVER_I2C_ADDR.
 *   - ESP_ERR_TIMEOUT     I2C bus timeout during probe or init sequence.
 *   - Other               ESP-IDF I2C driver error.
 */
esp_err_t i2c_ssd1306_init(i2c_master_bus_handle_t bus, i2c_ssd1306_handle_t *handle);

/**
 * @brief Deinitialise the SSD1306 display.
 *
 * Removes the I2C device from the master bus and deletes the mutex.
 * The handle must not be used after this call returns.
 *
 * @param handle Pointer to an initialised handle.
 * @return ESP_OK or an ESP-IDF driver error code.
 */
esp_err_t i2c_ssd1306_deinit(i2c_ssd1306_handle_t *handle);

/* =========================================================================
 * Convenience
 * ====================================================================== */

/**
 * @brief Clear the display (all pixels off) and flush to hardware.
 *
 * Equivalent to i2c_ssd1306_buffer_fill(handle, false) followed by
 * i2c_ssd1306_buffer_to_ram(handle). Provided as a convenience wrapper for
 * the common "blank the screen" operation.
 *
 * @param handle Pointer to an initialised handle.
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_ssd1306_clear(i2c_ssd1306_handle_t *handle);

/* =========================================================================
 * Buffer manipulation
 *
 * None of these functions touch the I2C bus. They modify the pixel buffer
 * in RAM and update the dirty-page bitmask. Call one of the _to_ram
 * functions afterwards to push changes to the display.
 * ====================================================================== */

/**
 * @brief Fill or clear the entire pixel buffer.
 *
 * Sets every byte in the active display region to 0xFF (all pixels on) when
 * @p fill is true, or 0x00 (all pixels off) when false. Marks all pages
 * dirty.
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param fill   true → all pixels on; false → all pixels off.
 * @return ESP_OK.
 */
esp_err_t i2c_ssd1306_buffer_fill(i2c_ssd1306_handle_t *handle, bool fill);

/**
 * @brief Set or clear a single pixel in the buffer.
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param x      Pixel column (0 … CONFIG_ESP_SSD1306_DRIVER_WIDTH-1).
 * @param y      Pixel row    (0 … CONFIG_ESP_SSD1306_DRIVER_HEIGHT-1).
 * @param fill   true → pixel on; false → pixel off.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if (x, y) is out of range.
 */
esp_err_t i2c_ssd1306_buffer_fill_pixel(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, bool fill);

/**
 * @brief Fill or clear a rectangular region of the buffer.
 *
 * Sets or clears all pixels inside the rectangle (x1, y1)–(x2, y2),
 * inclusive.
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param x1     Left   edge (0 … width-1,  must be ≤ x2).
 * @param x2     Right  edge (0 … width-1).
 * @param y1     Top    edge (0 … height-1, must be ≤ y2).
 * @param y2     Bottom edge (0 … height-1).
 * @param fill   true → pixels on; false → pixels off.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if coordinates are out of range.
 */
esp_err_t i2c_ssd1306_buffer_fill_space(i2c_ssd1306_handle_t *handle, uint8_t x1, uint8_t x2, uint8_t y1, uint8_t y2, bool fill);

/**
 * @brief Render text into the buffer (OR mode).
 *
 * Writes 8×8 font characters into the buffer using bitwise OR, preserving
 * any pixels already set. Use i2c_ssd1306_buffer_text_overwrite() when you
 * need to overwrite a region cleanly without a full buffer_fill().
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param x      Starting pixel column.
 * @param y      Starting pixel row.
 * @param text   Null-terminated string to render.
 * @param invert If true the character colours are inverted.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if text is NULL/empty or out of range.
 */
esp_err_t i2c_ssd1306_buffer_text(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, const char *text, bool invert);

/**
 * @brief Clear the text bounding box, then render text into the buffer.
 *
 * Erases the pixel rectangle occupied by @p text (width = strlen × 8 px,
 * height = 8 px) and then writes the characters. This lets a single line
 * be updated cleanly without clearing the whole screen first.
 *
 * Typical partial-update pattern:
 * @code
 *   i2c_ssd1306_buffer_text_overwrite(&ssd, 0, 12, new_value, false);
 *   i2c_ssd1306_region_to_ram(&ssd, 12, 19);   // ≈128 bytes sent, not 1024
 * @endcode
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param x      Starting pixel column.
 * @param y      Starting pixel row.
 * @param text   Null-terminated string to render.
 * @param invert If true the character colours are inverted.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if text is NULL/empty or out of range.
 */
esp_err_t i2c_ssd1306_buffer_text_overwrite(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, const char *text, bool invert);

/**
 * @brief Render a signed integer into the buffer.
 *
 * Formats @p value as a decimal string and calls i2c_ssd1306_buffer_text().
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param x      Starting pixel column.
 * @param y      Starting pixel row.
 * @param value  Value to render.
 * @param invert If true the digits are rendered inverted.
 * @return ESP_OK, or an error from i2c_ssd1306_buffer_text().
 */
esp_err_t i2c_ssd1306_buffer_int(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, int value, bool invert);

/**
 * @brief Clear the text bounding box, then render a signed integer.
 *
 * Formats @p value as a decimal string, erases the pixel rectangle it
 * occupies, and renders it. Use instead of i2c_ssd1306_buffer_int() when
 * the value can change width across calls (e.g. 9 → 10 → 100), so that
 * leftover pixels from a wider previous value are always cleared.
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param x      Starting pixel column.
 * @param y      Starting pixel row.
 * @param value  Value to render.
 * @param invert If true the digits are rendered inverted.
 * @return ESP_OK, or an error from i2c_ssd1306_buffer_text_overwrite().
 */
esp_err_t i2c_ssd1306_buffer_int_overwrite(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, int value, bool invert);

#if CONFIG_ESP_SSD1306_DRIVER_ENABLE_FLOAT
/**
 * @brief Render a floating-point number into the buffer.
 *
 * Formats @p value with @p decimals decimal places and calls
 * i2c_ssd1306_buffer_text().
 *
 * @warning Enabling this function pulls in newlib's float-printf support
 *          (~3–5 KB of additional flash). Disable in Kconfig if not needed.
 *
 * Compiled only when CONFIG_ESP_SSD1306_DRIVER_ENABLE_FLOAT is set.
 *
 * @param handle   Pointer to the SSD1306 handle.
 * @param x        Starting pixel column.
 * @param y        Starting pixel row.
 * @param value    Value to render.
 * @param decimals Number of decimal places (values > 6 are clamped to 6).
 * @param invert   If true the digits are rendered inverted.
 * @return ESP_OK, or an error from i2c_ssd1306_buffer_text().
 */
esp_err_t i2c_ssd1306_buffer_float(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, float value, uint8_t decimals, bool invert);

/**
 * @brief Clear the text bounding box, then render a floating-point number.
 *
 * Formats @p value with @p decimals decimal places (clamped to 6), erases
 * the pixel rectangle it occupies, and renders it. Use instead of
 * i2c_ssd1306_buffer_float() when the rendered width can change across
 * calls (e.g. 9.9 → 10.0).
 *
 * @warning Enabling float functions pulls in newlib's float-printf support
 *          (~3–5 KB of additional flash). Disable in Kconfig if not needed.
 *
 * @param handle   Pointer to the SSD1306 handle.
 * @param x        Starting pixel column.
 * @param y        Starting pixel row.
 * @param value    Value to render.
 * @param decimals Number of decimal places (values > 6 are clamped to 6).
 * @param invert   If true the digits are rendered inverted.
 * @return ESP_OK, or an error from i2c_ssd1306_buffer_text_overwrite().
 */
esp_err_t i2c_ssd1306_buffer_float_overwrite(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, float value, uint8_t decimals, bool invert);

#endif /* CONFIG_ESP_SSD1306_DRIVER_ENABLE_FLOAT */

/**
 * @brief Render a printf-style formatted string into the buffer.
 *
 * Expands the format string into a fixed stack buffer of
 * ESP_SSD1306_DRIVER_PRINTF_BUF_SIZE bytes, then calls
 * i2c_ssd1306_buffer_text(). Output that exceeds the buffer is silently
 * truncated.
 *
 * The @c format attribute lets GCC/Clang type-check format strings at
 * compile time, turning mismatched specifiers into warnings.
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param x      Starting pixel column.
 * @param y      Starting pixel row.
 * @param invert If true the text is rendered inverted.
 * @param fmt    printf-compatible format string.
 * @param ...    Format arguments.
 * @return ESP_OK, or an error from i2c_ssd1306_buffer_text().
 */
esp_err_t i2c_ssd1306_buffer_printf(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, bool invert, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/**
 * @brief Clear the text bounding box, then render a printf-style string.
 *
 * Expands @p fmt into a stack buffer of ESP_SSD1306_DRIVER_PRINTF_BUF_SIZE
 * bytes, erases the pixel rectangle occupied by the result, and renders it.
 * Use instead of i2c_ssd1306_buffer_printf() when the formatted value can
 * change width across calls (e.g. a counter that grows from one to two
 * digits), so that leftover pixels from a wider previous string are always
 * cleared.
 *
 * Prefer this over the two-call pattern below, which fails to clear old
 * wider content when the value shrinks:
 * @code
 *   // Bug-prone — "Counter: " area is never cleared:
 *   i2c_ssd1306_buffer_text_overwrite(&h, 0,  16, "Counter: ", false);
 *   i2c_ssd1306_buffer_printf(&h, 72, 16, false, "%d", i);
 *
 *   // Correct — clears the full formatted string as a single region:
 *   i2c_ssd1306_buffer_printf_overwrite(&h, 0, 16, false, "Counter: %d", i);
 * @endcode
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param x      Starting pixel column.
 * @param y      Starting pixel row.
 * @param invert If true the text is rendered inverted.
 * @param fmt    printf-compatible format string.
 * @param ...    Format arguments.
 * @return ESP_OK, or an error from i2c_ssd1306_buffer_text_overwrite().
 */
esp_err_t i2c_ssd1306_buffer_printf_overwrite(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, bool invert, const char *fmt, ...) __attribute__((format(printf, 5, 6)));

/**
 * @brief Render a bitmap image into the buffer.
 *
 * Copies image data stored in page-major, column-minor order into the
 * buffer. Pixels that fall outside the display boundary are clipped.
 *
 * @param handle     Pointer to the SSD1306 handle.
 * @param x          Starting pixel column.
 * @param y          Starting pixel row.
 * @param image      Pointer to the image data.
 * @param img_width  Image width in pixels.
 * @param img_height Image height in pixels (should be a multiple of 8).
 * @param invert     If true the image is rendered with inverted colours.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if image is NULL or coordinates are invalid.
 */
esp_err_t i2c_ssd1306_buffer_image(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, const uint8_t *image, uint8_t img_width, uint8_t img_height, bool invert);

/* =========================================================================
 * Buffer → display RAM transfer
 *
 * These functions push bytes from the RAM buffer to the SSD1306 display RAM
 * over I2C. Each page transfer is a single atomic, mutex-protected I2C
 * sequence (address command + data).
 * ====================================================================== */

/**
 * @brief Transfer a single page segment to the display RAM.
 *
 * Sends one byte from the buffer at (page, segment) over I2C. The dirty
 * bit for the page is cleared atomically before the transfer begins and
 * restored if the transfer fails, so any concurrent buffer write during
 * the I2C operation is not silently lost.
 *
 * @param handle  Pointer to the SSD1306 handle.
 * @param page    Page number (0 … ESP_SSD1306_DRIVER_TOTAL_PAGES-1).
 * @param segment Column within the page (0 … CONFIG_ESP_SSD1306_DRIVER_WIDTH-1).
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_ssd1306_segment_to_ram(i2c_ssd1306_handle_t *handle, uint8_t page, uint8_t segment);

/**
 * @brief Transfer a contiguous range of segments to the display RAM.
 *
 * Sends columns [initial_segment … final_segment] of @p page in a single
 * I2C data transaction. The dirty bit is cleared atomically before the
 * transfer and restored on failure.
 *
 * @param handle          Pointer to the SSD1306 handle.
 * @param page            Page number.
 * @param initial_segment First column to send (must be ≤ final_segment).
 * @param final_segment   Last column to send.
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_ssd1306_segments_to_ram(i2c_ssd1306_handle_t *handle, uint8_t page, uint8_t initial_segment, uint8_t final_segment);

/**
 * @brief Transfer one full page to the display RAM.
 *
 * Sends all CONFIG_ESP_SSD1306_DRIVER_WIDTH bytes of @p page in a single
 * I2C transaction. The dirty bit is cleared atomically before the transfer
 * and restored on failure.
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param page   Page number (0 … ESP_SSD1306_DRIVER_TOTAL_PAGES-1).
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_ssd1306_page_to_ram(i2c_ssd1306_handle_t *handle, uint8_t page);

/**
 * @brief Transfer a range of pages to the display RAM.
 *
 * Calls i2c_ssd1306_page_to_ram() for each page in
 * [initial_page … final_page]. Returns immediately on the first error.
 *
 * @param handle       Pointer to the SSD1306 handle.
 * @param initial_page First page to send (must be ≤ final_page).
 * @param final_page   Last page to send.
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_ssd1306_pages_to_ram(i2c_ssd1306_handle_t *handle, uint8_t initial_page, uint8_t final_page);

/**
 * @brief Transfer only the pages that overlap a vertical pixel range.
 *
 * Computes the first and last pages that cover rows [y1 … y2] and flushes
 * only those pages. For a single 8-pixel text line this sends ~128 bytes
 * instead of the full 1 024 bytes, eliminating flicker from unnecessary I2C
 * traffic.
 *
 * @param handle Pointer to the SSD1306 handle.
 * @param y1     Top pixel row of the region (0 … CONFIG_ESP_SSD1306_DRIVER_HEIGHT-1).
 * @param y2     Bottom pixel row            (must be ≥ y1).
 * @return ESP_OK or an I2C driver error.
 */
esp_err_t i2c_ssd1306_region_to_ram(i2c_ssd1306_handle_t *handle, uint8_t y1, uint8_t y2);

/**
 * @brief Transfer only the dirty pages to the display RAM.
 *
 * Takes a snapshot of the dirty-page bitmask and flushes each marked page.
 * Each dirty bit is cleared atomically before its page is transmitted;
 * any concurrent buffer modification during the I2C transfer re-sets the
 * bit so the page is retried on the next call. Failed pages also remain
 * dirty for retry.
 *
 * This is the recommended flush function for most use cases.
 *
 * @param handle Pointer to the SSD1306 handle.
 * @return ESP_OK if all dirty pages were sent, or the first I2C error encountered.
 */
esp_err_t i2c_ssd1306_buffer_to_ram(i2c_ssd1306_handle_t *handle);