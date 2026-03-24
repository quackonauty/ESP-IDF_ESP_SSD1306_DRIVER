#define LOG_LOCAL_LEVEL CONFIG_ESP_SSD1306_DRIVER_LOG_LEVEL

#include "esp_ssd1306_driver.h"
#include "esp_ssd1306_const.h"

#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>

#include <esp_log.h>

static const char *TAG = "ESP_SSD1306_DRIVER";

/* =========================================================================
 * Internal helpers
 * ====================================================================== */

/**
 * @brief Compute the page bitmask that covers pixel rows [y1 … y2].
 *
 * Bit N in the returned mask is set if page N contains at least one row in
 * the range. Used by buffer-write functions to mark the correct pages dirty.
 */
static inline uint8_t ssd1306_page_mask_for_rows(uint8_t y1, uint8_t y2)
{
    uint8_t mask = 0;
    uint8_t first = y1 / 8u;
    uint8_t last = y2 / 8u;
    if (last >= ESP_SSD1306_DRIVER_TOTAL_PAGES)
        last = ESP_SSD1306_DRIVER_TOTAL_PAGES - 1u;
    for (uint8_t p = first; p <= last; p++)
        mask |= (uint8_t)(1u << p);
    return mask;
}

/**
 * @brief Atomically mark one or more pages as dirty.
 *
 * Uses a relaxed atomic OR so no bit-set from a concurrent task is lost.
 */
static inline void ssd1306_mark_dirty(i2c_ssd1306_handle_t *h, uint8_t page_mask)
{
    atomic_fetch_or_explicit(&h->dirty_pages, page_mask, memory_order_relaxed);
}

/**
 * @brief Transmit a buffer over I2C without acquiring the mutex.
 *
 * The caller must hold h->mutex before calling this function.
 */
static esp_err_t ssd1306_raw_transmit(i2c_ssd1306_handle_t *h, const uint8_t *data, size_t len)
{
    TickType_t ticks = pdMS_TO_TICKS(ESP_SSD1306_DRIVER_TIMEOUT_MS);
    return i2c_master_transmit(h->i2c_master_dev, data, len, ticks);
}

/**
 * @brief Take the mutex, send a single buffer, release the mutex.
 *
 * Used for one-shot transmissions such as the init sequence.
 */
static esp_err_t ssd1306_locked_transmit(i2c_ssd1306_handle_t *h, const uint8_t *data, size_t len)
{
    if (xSemaphoreTake(h->mutex, pdMS_TO_TICKS(ESP_SSD1306_DRIVER_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ssd1306_raw_transmit(h, data, len);
    xSemaphoreGive(h->mutex);
    return ret;
}

/**
 * @brief Take the mutex, send two buffers in sequence, release the mutex.
 *
 * Used for address-command + data pairs so no other task can insert an I2C
 * transaction between the page-address command and the pixel data.
 *
 * Without this, a concurrent task calling page_to_ram on a different page
 * could interleave its address command between this function's address and
 * data commands, sending data to the wrong page on the display.
 */
static esp_err_t ssd1306_locked_transmit_pair(i2c_ssd1306_handle_t *h, const uint8_t *cmd, size_t cmd_len, const uint8_t *data, size_t data_len)
{
    if (xSemaphoreTake(h->mutex, pdMS_TO_TICKS(ESP_SSD1306_DRIVER_TIMEOUT_MS)) != pdTRUE)
    {
        ESP_LOGE(TAG, "Mutex timeout");
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ssd1306_raw_transmit(h, cmd, cmd_len);
    if (ret == ESP_OK)
        ret = ssd1306_raw_transmit(h, data, data_len);
    xSemaphoreGive(h->mutex);
    return ret;
}

/**
 * @brief Shared implementation for buffer_printf and buffer_printf_overwrite.
 *
 * Expands @p fmt into a stack buffer of ESP_SSD1306_DRIVER_PRINTF_BUF_SIZE
 * bytes using the provided @p args, then delegates to either
 * i2c_ssd1306_buffer_text() or i2c_ssd1306_buffer_text_overwrite() depending
 * on @p overwrite. Output that exceeds the buffer is silently truncated.
 *
 * @note @p args must be initialized with va_start() by the caller and cleaned
 *       up with va_end() after this function returns. va_start/va_end must
 *       always remain in the same function that declares the va_list.
 */
static esp_err_t ssd1306_printf_impl(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, bool invert, bool overwrite, const char *fmt, va_list args)
{
    char text[ESP_SSD1306_DRIVER_PRINTF_BUF_SIZE];
    vsnprintf(text, sizeof(text), fmt, args);

    return overwrite ? i2c_ssd1306_buffer_text_overwrite(handle, x, y, text, invert) : i2c_ssd1306_buffer_text(handle, x, y, text, invert);
}

/* =========================================================================
 * Lifecycle
 * ====================================================================== */

esp_err_t i2c_ssd1306_init(i2c_master_bus_handle_t bus, i2c_ssd1306_handle_t *handle)
{
    ESP_LOGI(TAG, "Initializing SSD1306 (%ux%u) at I2C addr 0x%02X, %lu Hz", CONFIG_ESP_SSD1306_DRIVER_WIDTH, CONFIG_ESP_SSD1306_DRIVER_HEIGHT, (unsigned)CONFIG_ESP_SSD1306_DRIVER_I2C_ADDR, (unsigned long)CONFIG_ESP_SSD1306_DRIVER_I2C_SCL_SPEED_HZ);

    handle->mutex = xSemaphoreCreateMutex();
    if (handle->mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex (out of heap)");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = i2c_master_probe(bus, CONFIG_ESP_SSD1306_DRIVER_I2C_ADDR, ESP_SSD1306_DRIVER_TIMEOUT_MS / portTICK_PERIOD_MS);
    if (ret != ESP_OK)
    {
        switch (ret)
        {
        case ESP_ERR_NOT_FOUND:
            ESP_LOGE(TAG, "No device at I2C address 0x%02X", (unsigned)CONFIG_ESP_SSD1306_DRIVER_I2C_ADDR);
            break;
        case ESP_ERR_TIMEOUT:
            ESP_LOGE(TAG, "I2C timeout probing address 0x%02X", (unsigned)CONFIG_ESP_SSD1306_DRIVER_I2C_ADDR);
            break;
        default:
            ESP_LOGE(TAG, "I2C probe error at 0x%02X (0x%x)", (unsigned)CONFIG_ESP_SSD1306_DRIVER_I2C_ADDR, ret);
            break;
        }
        goto cleanup_mutex;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_7,
        .device_address = CONFIG_ESP_SSD1306_DRIVER_I2C_ADDR,
        .scl_speed_hz = CONFIG_ESP_SSD1306_DRIVER_I2C_SCL_SPEED_HZ,
    };

    ret = i2c_master_bus_add_device(bus, &dev_cfg, &handle->i2c_master_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register I2C device (0x%x)", ret);
        goto cleanup_mutex;
    }

    uint8_t init_cmd[] = {
        OLED_CONTROL_BYTE_CMD,
        OLED_CMD_DISPLAY_OFF,
        OLED_CMD_SET_MUX_RATIO,
        (CONFIG_ESP_SSD1306_DRIVER_HEIGHT - 1u),
        OLED_CMD_SET_VERT_DISPLAY_OFFSET,
        0x00,
        OLED_MASK_DISPLAY_START_LINE | 0x00,
        OLED_CMD_COM_SCAN_DIRECTION_NORMAL,
        OLED_CMD_SEGMENT_REMAP_LEFT_TO_RIGHT,
        OLED_CMD_SET_COM_PIN_HARDWARE_MAP,
        CONFIG_ESP_SSD1306_DRIVER_COM_PIN_CFG,
        OLED_CMD_SET_MEMORY_ADDR_MODE,
        0x02,
        OLED_CMD_SET_CONTRAST_CONTROL,
        0xFF,
        OLED_CMD_SET_DISPLAY_CLK_DIVIDE,
        0x80,
        OLED_CMD_ENABLE_DISPLAY_RAM,
        OLED_CMD_NORMAL_DISPLAY,
        OLED_CMD_SET_CHARGE_PUMP,
        0x14,
        OLED_CMD_DISPLAY_ON,
    };

#if CONFIG_ESP_SSD1306_DRIVER_FLIP
    init_cmd[7] = OLED_CMD_COM_SCAN_DIRECTION_REMAP;
    init_cmd[8] = OLED_CMD_SEGMENT_REMAP_RIGHT_TO_LEFT;
#endif

    ret = ssd1306_locked_transmit(handle, init_cmd, sizeof(init_cmd));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to send init sequence (0x%x)", ret);
        goto cleanup_device;
    }

    memset(handle->buffer, 0x00, sizeof(handle->buffer));
    atomic_store_explicit(&handle->dirty_pages, 0x00u, memory_order_relaxed);

    ESP_LOGI(TAG, "SSD1306 initialized");
    return ESP_OK;

cleanup_device:
    i2c_master_bus_rm_device(handle->i2c_master_dev);
cleanup_mutex:
    vSemaphoreDelete(handle->mutex);
    handle->mutex = NULL;
    return ret;
}

esp_err_t i2c_ssd1306_deinit(i2c_ssd1306_handle_t *handle)
{
    ESP_LOGI(TAG, "Deinitializing SSD1306");

    esp_err_t ret = i2c_master_bus_rm_device(handle->i2c_master_dev);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to remove I2C device (0x%x)", ret);
        return ret;
    }

    if (handle->mutex != NULL)
    {
        vSemaphoreDelete(handle->mutex);
        handle->mutex = NULL;
    }

    ESP_LOGI(TAG, "SSD1306 deinitialized");
    return ESP_OK;
}

/* =========================================================================
 * Convenience
 * ====================================================================== */

esp_err_t i2c_ssd1306_clear(i2c_ssd1306_handle_t *handle)
{
    esp_err_t ret = i2c_ssd1306_buffer_fill(handle, false);
    if (ret != ESP_OK)
        return ret;
    return i2c_ssd1306_buffer_to_ram(handle);
}

/* =========================================================================
 * Buffer manipulation
 * ====================================================================== */

esp_err_t i2c_ssd1306_buffer_fill(i2c_ssd1306_handle_t *handle, bool fill)
{
    memset(handle->buffer, fill ? 0xFF : 0x00, ESP_SSD1306_DRIVER_BUF_SIZE);

    uint8_t all_pages = (uint8_t)((1u << ESP_SSD1306_DRIVER_TOTAL_PAGES) - 1u);
    atomic_store_explicit(&handle->dirty_pages, all_pages, memory_order_relaxed);
    return ESP_OK;
}

esp_err_t i2c_ssd1306_buffer_fill_pixel(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, bool fill)
{
    if (x >= CONFIG_ESP_SSD1306_DRIVER_WIDTH || y >= CONFIG_ESP_SSD1306_DRIVER_HEIGHT)
    {
        ESP_LOGE(TAG, "Pixel out of range: (%u,%u), max (%u,%u)", x, y, CONFIG_ESP_SSD1306_DRIVER_WIDTH - 1u, CONFIG_ESP_SSD1306_DRIVER_HEIGHT - 1u);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t page = y / 8u;
    uint8_t bit = (uint8_t)(1u << (y % 8u));
    uint16_t index = (uint16_t)page * CONFIG_ESP_SSD1306_DRIVER_WIDTH + x;

    if (fill)
        handle->buffer[index] |= bit;
    else
        handle->buffer[index] &= (uint8_t)~bit;

    ssd1306_mark_dirty(handle, (uint8_t)(1u << page));
    return ESP_OK;
}

esp_err_t i2c_ssd1306_buffer_fill_space(i2c_ssd1306_handle_t *handle, uint8_t x1, uint8_t x2, uint8_t y1, uint8_t y2, bool fill)
{
    if (x1 >= CONFIG_ESP_SSD1306_DRIVER_WIDTH || x2 >= CONFIG_ESP_SSD1306_DRIVER_WIDTH || y1 >= CONFIG_ESP_SSD1306_DRIVER_HEIGHT || y2 >= CONFIG_ESP_SSD1306_DRIVER_HEIGHT)
    {
        ESP_LOGE(TAG, "Space out of range: x[%u..%u] y[%u..%u], max x=%u y=%u", x1, x2, y1, y2, CONFIG_ESP_SSD1306_DRIVER_WIDTH - 1u, CONFIG_ESP_SSD1306_DRIVER_HEIGHT - 1u);
        return ESP_ERR_INVALID_ARG;
    }
    if (x1 > x2 || y1 > y2)
    {
        ESP_LOGE(TAG, "Inverted coordinates: x[%u..%u] y[%u..%u]", x1, x2, y1, y2);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t start_page = y1 / 8u;
    uint8_t end_page = y2 / 8u;
    uint8_t offset_start = y1 % 8u;
    uint8_t offset_end = y2 % 8u;

    for (uint8_t page = start_page; page <= end_page; page++)
    {
        uint8_t mask;
        if (start_page == end_page)
            mask = (uint8_t)((0xFFu << offset_start) & (0xFFu >> (7u - offset_end)));
        else if (page == start_page)
            mask = (uint8_t)(0xFFu << offset_start);
        else if (page == end_page)
            mask = (uint8_t)(0xFFu >> (7u - offset_end));
        else
            mask = 0xFFu;

        for (uint8_t col = x1; col <= x2; col++)
        {
            uint16_t index = (uint16_t)page * CONFIG_ESP_SSD1306_DRIVER_WIDTH + col;
            if (fill)
                handle->buffer[index] |= mask;
            else
                handle->buffer[index] &= (uint8_t)~mask;
        }
    }

    ssd1306_mark_dirty(handle, ssd1306_page_mask_for_rows(y1, y2));
    return ESP_OK;
}

esp_err_t i2c_ssd1306_buffer_text(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, const char *text, bool invert)
{
    if (text == NULL)
    {
        ESP_LOGE(TAG, "text is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (text[0] == '\0')
    {
        ESP_LOGW(TAG, "text is empty, nothing to render");
        return ESP_ERR_INVALID_ARG;
    }

    if (x >= CONFIG_ESP_SSD1306_DRIVER_WIDTH || y >= CONFIG_ESP_SSD1306_DRIVER_HEIGHT)
    {
        ESP_LOGE(TAG, "Invalid text position: x=%u (max %u), y=%u (max %u)", x, CONFIG_ESP_SSD1306_DRIVER_WIDTH - 1u, y, CONFIG_ESP_SSD1306_DRIVER_HEIGHT - 1u);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t page = y / 8u;
    uint8_t offset = y % 8u;
    bool has_next = (page + 1u) < ESP_SSD1306_DRIVER_TOTAL_PAGES;

    size_t text_len = strlen(text);
    uint8_t chars_visible = (CONFIG_ESP_SSD1306_DRIVER_WIDTH - x) / 8u;
    if ((x + (uint16_t)text_len * 8u) > CONFIG_ESP_SSD1306_DRIVER_WIDTH)
        ESP_LOGW(TAG, "Text truncated: %u of %u chars visible", chars_visible, (uint8_t)text_len);

    if (offset != 0u && !has_next)
        ESP_LOGW(TAG, "Text vertically truncated: y=%u causes overflow", y);

    uint8_t cur_x = x;
    for (size_t i = 0; text[i] != '\0' && cur_x < CONFIG_ESP_SSD1306_DRIVER_WIDTH; i++)
    {
        const uint8_t *glyph = font8x8[(uint8_t)text[i]];
        uint8_t cols = (uint8_t)((CONFIG_ESP_SSD1306_DRIVER_WIDTH - cur_x) < 8u ? (CONFIG_ESP_SSD1306_DRIVER_WIDTH - cur_x) : 8u);

        for (uint8_t j = 0; j < cols; j++)
        {
            uint8_t col_data = invert ? (uint8_t)~glyph[j] : glyph[j];
            uint16_t index = (uint16_t)page * CONFIG_ESP_SSD1306_DRIVER_WIDTH + cur_x + j;

            if (offset == 0u)
            {
                handle->buffer[index] |= col_data;
            }
            else
            {
                handle->buffer[index] |= (uint8_t)(col_data << offset);
                if (has_next)
                    handle->buffer[index + CONFIG_ESP_SSD1306_DRIVER_WIDTH] |= (uint8_t)(col_data >> (8u - offset));
            }
        }
        cur_x += 8u;
    }

    ssd1306_mark_dirty(handle, ssd1306_page_mask_for_rows(y, (uint8_t)(y + 7u)));
    return ESP_OK;
}

esp_err_t i2c_ssd1306_buffer_text_overwrite(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, const char *text, bool invert)
{
    if (text == NULL)
    {
        ESP_LOGE(TAG, "text is NULL");
        return ESP_ERR_INVALID_ARG;
    }
    if (text[0] == '\0')
    {
        ESP_LOGW(TAG, "text is empty, nothing to render");
        return ESP_ERR_INVALID_ARG;
    }
    if (x >= CONFIG_ESP_SSD1306_DRIVER_WIDTH || y >= CONFIG_ESP_SSD1306_DRIVER_HEIGHT)
    {
        ESP_LOGE(TAG, "Invalid text position: x=%u (max %u), y=%u (max %u)", x, CONFIG_ESP_SSD1306_DRIVER_WIDTH - 1u, y, CONFIG_ESP_SSD1306_DRIVER_HEIGHT - 1u);
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t text_w = (uint16_t)strlen(text) * 8u;
    uint8_t x2 = (x + text_w > (unsigned)CONFIG_ESP_SSD1306_DRIVER_WIDTH) ? (uint8_t)(CONFIG_ESP_SSD1306_DRIVER_WIDTH - 1u) : (uint8_t)(x + text_w - 1u);
    uint8_t y2 = ((unsigned)(y + 7u) < (unsigned)CONFIG_ESP_SSD1306_DRIVER_HEIGHT) ? (uint8_t)(y + 7u) : (uint8_t)(CONFIG_ESP_SSD1306_DRIVER_HEIGHT - 1u);

    esp_err_t ret = i2c_ssd1306_buffer_fill_space(handle, x, x2, y, y2, false);
    if (ret != ESP_OK)
        return ret;

    return i2c_ssd1306_buffer_text(handle, x, y, text, invert);
}

esp_err_t i2c_ssd1306_buffer_int(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, int value, bool invert)
{
    char text[12];
    snprintf(text, sizeof(text), "%d", value);
    return i2c_ssd1306_buffer_text(handle, x, y, text, invert);
}

esp_err_t i2c_ssd1306_buffer_int_overwrite(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, int value, bool invert)
{
    char text[12];
    snprintf(text, sizeof(text), "%d", value);
    return i2c_ssd1306_buffer_text_overwrite(handle, x, y, text, invert);
}

#if CONFIG_ESP_SSD1306_DRIVER_ENABLE_FLOAT
esp_err_t i2c_ssd1306_buffer_float(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, float value, uint8_t decimals, bool invert)
{
    if (decimals > 6)
    {
        ESP_LOGW(TAG, "decimals=%u clamped to 6", decimals);
        decimals = 6;
    }

    char text[32];
    snprintf(text, sizeof(text), "%.*f", (int)decimals, value);
    return i2c_ssd1306_buffer_text(handle, x, y, text, invert);
}

esp_err_t i2c_ssd1306_buffer_float_overwrite(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, float value, uint8_t decimals, bool invert)
{
    if (decimals > 6)
    {
        ESP_LOGW(TAG, "decimals=%u clamped to 6", decimals);
        decimals = 6;
    }

    char text[32];
    snprintf(text, sizeof(text), "%.*f", (int)decimals, value);
    return i2c_ssd1306_buffer_text_overwrite(handle, x, y, text, invert);
}
#endif /* CONFIG_ESP_SSD1306_DRIVER_ENABLE_FLOAT */

esp_err_t i2c_ssd1306_buffer_printf(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, bool invert, const char *fmt, ...)
{
    if (fmt == NULL)
    {
        ESP_LOGE(TAG, "NULL format string");
        return ESP_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, fmt);
    esp_err_t ret = ssd1306_printf_impl(handle, x, y, invert, false, fmt, args);
    va_end(args);
    return ret;
}

esp_err_t i2c_ssd1306_buffer_printf_overwrite(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, bool invert, const char *fmt, ...)
{
    if (fmt == NULL)
    {
        ESP_LOGE(TAG, "NULL format string");
        return ESP_ERR_INVALID_ARG;
    }

    va_list args;
    va_start(args, fmt);
    esp_err_t ret = ssd1306_printf_impl(handle, x, y, invert, true, fmt, args);
    va_end(args);
    return ret;
}

esp_err_t i2c_ssd1306_buffer_image(i2c_ssd1306_handle_t *handle, uint8_t x, uint8_t y, const uint8_t *image, uint8_t img_width, uint8_t img_height, bool invert)
{
    if (image == NULL)
    {
        ESP_LOGE(TAG, "image is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (img_width == 0u || img_height == 0u)
    {
        ESP_LOGE(TAG, "Invalid image dimensions: %ux%u", img_width, img_height);
        return ESP_ERR_INVALID_ARG;
    }

    if (x >= CONFIG_ESP_SSD1306_DRIVER_WIDTH || y >= CONFIG_ESP_SSD1306_DRIVER_HEIGHT)
    {
        ESP_LOGE(TAG, "Image position out of range: x=%u (max %u), y=%u (max %u)", x, CONFIG_ESP_SSD1306_DRIVER_WIDTH - 1u, y, CONFIG_ESP_SSD1306_DRIVER_HEIGHT - 1u);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t draw_w = (img_width < (CONFIG_ESP_SSD1306_DRIVER_WIDTH - x)) ? img_width : (uint8_t)(CONFIG_ESP_SSD1306_DRIVER_WIDTH - x);
    uint8_t draw_h = (img_height < (CONFIG_ESP_SSD1306_DRIVER_HEIGHT - y)) ? img_height : (uint8_t)(CONFIG_ESP_SSD1306_DRIVER_HEIGHT - y);

    if (img_width > draw_w)
        ESP_LOGW(TAG, "Image horizontally clipped: %u of %u columns visible", draw_w, img_width);
    if (img_height > draw_h)
        ESP_LOGW(TAG, "Image vertically clipped: %u of %u rows visible", draw_h, img_height);

    uint8_t start_page = y / 8u;
    uint8_t vert_offset = y % 8u;
    uint8_t draw_pages = (draw_h + 7u) / 8u;

    for (uint8_t col = 0; col < draw_w; col++)
    {
        for (uint8_t page = 0; page < draw_pages; page++)
        {
            uint8_t target = start_page + page;
            if (target >= ESP_SSD1306_DRIVER_TOTAL_PAGES)
                break;

            uint8_t byte = image[(uint16_t)page * img_width + col];
            if (invert)
                byte = (uint8_t)~byte;

            uint16_t idx = (uint16_t)target * CONFIG_ESP_SSD1306_DRIVER_WIDTH + (x + col);

            if (vert_offset == 0u)
            {
                handle->buffer[idx] |= byte;
            }
            else
            {
                handle->buffer[idx] |= (uint8_t)(byte << vert_offset);
                if (target + 1u < ESP_SSD1306_DRIVER_TOTAL_PAGES)
                    handle->buffer[idx + CONFIG_ESP_SSD1306_DRIVER_WIDTH] |= (uint8_t)(byte >> (8u - vert_offset));
            }
        }
    }

    ssd1306_mark_dirty(handle, ssd1306_page_mask_for_rows(y, (uint8_t)(y + draw_h - 1u)));
    return ESP_OK;
}

/* =========================================================================
 * Buffer → display RAM transfer
 * ====================================================================== */

esp_err_t i2c_ssd1306_segment_to_ram(i2c_ssd1306_handle_t *handle, uint8_t page, uint8_t segment)
{
    if (page >= ESP_SSD1306_DRIVER_TOTAL_PAGES)
    {
        ESP_LOGE(TAG, "segment_to_ram: page %u out of range (max %u)", page, ESP_SSD1306_DRIVER_TOTAL_PAGES - 1u);
        return ESP_ERR_INVALID_ARG;
    }
    if (segment >= CONFIG_ESP_SSD1306_DRIVER_WIDTH)
    {
        ESP_LOGE(TAG, "segment_to_ram: segment %u out of range (max %u)", segment, CONFIG_ESP_SSD1306_DRIVER_WIDTH - 1u);
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t addr_cmd[] = {
        OLED_CONTROL_BYTE_CMD,
        (uint8_t)(OLED_MASK_PAGE_ADDR | page),
        (uint8_t)(OLED_MASK_LSB_NIBBLE_SEG_ADDR | (segment & 0x0Fu)),
        (uint8_t)(OLED_MASK_HSB_NIBBLE_SEG_ADDR | ((segment >> 4u) & 0x0Fu)),
    };

    atomic_fetch_and_explicit(&handle->dirty_pages, (uint8_t)~(1u << page), memory_order_relaxed);

    uint16_t index = (uint16_t)page * CONFIG_ESP_SSD1306_DRIVER_WIDTH + segment;
    const uint8_t data_cmd[] = {
        OLED_CONTROL_BYTE_DATA,
        handle->buffer[index],
    };

    esp_err_t err = ssd1306_locked_transmit_pair(handle, addr_cmd, sizeof(addr_cmd), data_cmd, sizeof(data_cmd));
    if (err != ESP_OK)
    {
        atomic_fetch_or_explicit(&handle->dirty_pages, (uint8_t)(1u << page), memory_order_relaxed);
        ESP_LOGE(TAG, "segment_to_ram failed (0x%x)", err);
    }
    return err;
}

esp_err_t i2c_ssd1306_segments_to_ram(i2c_ssd1306_handle_t *handle, uint8_t page, uint8_t initial_segment, uint8_t final_segment)
{
    if (page >= ESP_SSD1306_DRIVER_TOTAL_PAGES)
    {
        ESP_LOGE(TAG, "segments_to_ram: page %u out of range (max %u)", page, ESP_SSD1306_DRIVER_TOTAL_PAGES - 1u);
        return ESP_ERR_INVALID_ARG;
    }
    if (initial_segment >= CONFIG_ESP_SSD1306_DRIVER_WIDTH ||
        final_segment >= CONFIG_ESP_SSD1306_DRIVER_WIDTH)
    {
        ESP_LOGE(TAG, "segments_to_ram: segment range %u..%u out of range (max %u)", initial_segment, final_segment, CONFIG_ESP_SSD1306_DRIVER_WIDTH - 1u);
        return ESP_ERR_INVALID_ARG;
    }
    if (initial_segment > final_segment)
    {
        ESP_LOGE(TAG, "segments_to_ram: inverted segment range %u..%u", initial_segment, final_segment);
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t addr_cmd[] = {
        OLED_CONTROL_BYTE_CMD,
        (uint8_t)(OLED_MASK_PAGE_ADDR | page),
        (uint8_t)(OLED_MASK_LSB_NIBBLE_SEG_ADDR | (initial_segment & 0x0Fu)),
        (uint8_t)(OLED_MASK_HSB_NIBBLE_SEG_ADDR | ((initial_segment >> 4u) & 0x0Fu)),
    };

    uint8_t count = final_segment - initial_segment + 1u;

    atomic_fetch_and_explicit(&handle->dirty_pages, (uint8_t)~(1u << page), memory_order_relaxed);

    uint8_t data_cmd[CONFIG_ESP_SSD1306_DRIVER_WIDTH + 1u];
    data_cmd[0] = OLED_CONTROL_BYTE_DATA;
    memcpy(&data_cmd[1], &handle->buffer[(uint16_t)page * CONFIG_ESP_SSD1306_DRIVER_WIDTH + initial_segment], count);

    esp_err_t err = ssd1306_locked_transmit_pair(handle, addr_cmd, sizeof(addr_cmd), data_cmd, (size_t)count + 1u);
    if (err != ESP_OK)
    {
        atomic_fetch_or_explicit(&handle->dirty_pages, (uint8_t)(1u << page), memory_order_relaxed);
        ESP_LOGE(TAG, "segments_to_ram failed (0x%x)", err);
    }
    return err;
}

esp_err_t i2c_ssd1306_page_to_ram(i2c_ssd1306_handle_t *handle, uint8_t page)
{
    if (page >= ESP_SSD1306_DRIVER_TOTAL_PAGES)
    {
        ESP_LOGE(TAG, "page_to_ram: page %u out of range (max %u)", page, ESP_SSD1306_DRIVER_TOTAL_PAGES - 1u);
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t addr_cmd[] = {
        OLED_CONTROL_BYTE_CMD,
        (uint8_t)(OLED_MASK_PAGE_ADDR | page),
        (uint8_t)(OLED_MASK_LSB_NIBBLE_SEG_ADDR | 0x00u),
        (uint8_t)(OLED_MASK_HSB_NIBBLE_SEG_ADDR | 0x00u),
    };

    atomic_fetch_and_explicit(&handle->dirty_pages, (uint8_t)~(1u << page), memory_order_relaxed);

    uint8_t data_cmd[CONFIG_ESP_SSD1306_DRIVER_WIDTH + 1u];
    data_cmd[0] = OLED_CONTROL_BYTE_DATA;
    memcpy(&data_cmd[1], &handle->buffer[(uint16_t)page * CONFIG_ESP_SSD1306_DRIVER_WIDTH], CONFIG_ESP_SSD1306_DRIVER_WIDTH);

    esp_err_t err = ssd1306_locked_transmit_pair(handle, addr_cmd, sizeof(addr_cmd), data_cmd, CONFIG_ESP_SSD1306_DRIVER_WIDTH + 1u);
    if (err != ESP_OK)
    {
        atomic_fetch_or_explicit(&handle->dirty_pages, (uint8_t)(1u << page), memory_order_relaxed);
        ESP_LOGE(TAG, "page_to_ram: page %u failed (0x%x)", page, err);
    }
    return err;
}

esp_err_t i2c_ssd1306_pages_to_ram(i2c_ssd1306_handle_t *handle, uint8_t initial_page, uint8_t final_page)
{
    if (initial_page >= ESP_SSD1306_DRIVER_TOTAL_PAGES || final_page >= ESP_SSD1306_DRIVER_TOTAL_PAGES)
    {
        ESP_LOGE(TAG, "pages_to_ram: page range %u..%u out of range (max %u)", initial_page, final_page, ESP_SSD1306_DRIVER_TOTAL_PAGES - 1u);
        return ESP_ERR_INVALID_ARG;
    }
    if (initial_page > final_page)
    {
        ESP_LOGE(TAG, "pages_to_ram: inverted page range %u..%u", initial_page, final_page);
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    for (uint8_t p = initial_page; p <= final_page; p++)
    {
        err = i2c_ssd1306_page_to_ram(handle, p);
        if (err != ESP_OK)
            return err;
    }
    return err;
}

esp_err_t i2c_ssd1306_region_to_ram(i2c_ssd1306_handle_t *handle, uint8_t y1, uint8_t y2)
{
    if (y1 >= CONFIG_ESP_SSD1306_DRIVER_HEIGHT || y2 >= CONFIG_ESP_SSD1306_DRIVER_HEIGHT)
    {
        ESP_LOGE(TAG, "region_to_ram: y range %u..%u out of range (max %u)", y1, y2, CONFIG_ESP_SSD1306_DRIVER_HEIGHT - 1u);
        return ESP_ERR_INVALID_ARG;
    }
    if (y1 > y2)
    {
        ESP_LOGE(TAG, "region_to_ram: inverted y range %u..%u", y1, y2);
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t first = y1 / 8u;
    uint8_t last = y2 / 8u;
    if (last >= ESP_SSD1306_DRIVER_TOTAL_PAGES)
        last = ESP_SSD1306_DRIVER_TOTAL_PAGES - 1u;

    return i2c_ssd1306_pages_to_ram(handle, first, last);
}

esp_err_t i2c_ssd1306_buffer_to_ram(i2c_ssd1306_handle_t *handle)
{
    uint8_t dirty = atomic_load_explicit(&handle->dirty_pages, memory_order_relaxed);
    esp_err_t err = ESP_OK;

    for (uint8_t p = 0; p < ESP_SSD1306_DRIVER_TOTAL_PAGES; p++)
    {
        if (dirty & (uint8_t)(1u << p))
        {
            err = i2c_ssd1306_page_to_ram(handle, p);
            if (err != ESP_OK)
                return err;
        }
    }
    return err;
}