#pragma once
#ifndef ESP32SPI_SLAVE_H
#define ESP32SPI_SLAVE_H

#include <Arduino.h>
#include <SPI.h>
#include <driver/spi_slave.h>
#include <soc/soc_caps.h>
#include <vector>
#include <string>

#ifndef ARDUINO_ESP32_SPI_SLAVE_NAMESPACE_BEGIN
#define ARDUINO_ESP32_SPI_SLAVE_NAMESPACE_BEGIN \
    namespace arduino {                         \
    namespace esp32 {                           \
        namespace spi {                         \
            namespace slave {
#endif
#ifndef ARDUINO_ESP32_SPI_SLAVE_NAMESPACE_END
#define ARDUINO_ESP32_SPI_SLAVE_NAMESPACE_END \
    }                                         \
    }                                         \
    }                                         \
    }
#endif

ARDUINO_ESP32_SPI_SLAVE_NAMESPACE_BEGIN

static constexpr const char *TAG = "ESP32SPISlave";
static constexpr int SPI_SLAVE_TASK_STASCK_SIZE = 1024 * 2;
static constexpr int SPI_SLAVE_TASK_PRIORITY = 5;

static QueueHandle_t s_trans_queue_handle {NULL};
static constexpr int SEND_TRANS_QUEUE_TIMEOUT_TICKS = pdMS_TO_TICKS(5000);
static constexpr int RECV_TRANS_QUEUE_TIMEOUT_TICKS = pdMS_TO_TICKS(5000);
static QueueHandle_t s_trans_result_handle {NULL};
static constexpr int SEND_TRANS_RESULT_TIMEOUT_TICKS = pdMS_TO_TICKS(5000);
static constexpr int RECV_TRANS_RESULT_TIMEOUT_TICKS = 0;
static QueueHandle_t s_trans_error_handle {NULL};
static constexpr int SEND_TRANS_ERROR_TIMEOUT_TICKS = pdMS_TO_TICKS(5000);
static constexpr int RECV_TRANS_ERROR_TIMEOUT_TICKS = 0;
static QueueHandle_t s_in_flight_mailbox_handle {NULL};

using spi_slave_user_cb_t = std::function<void(spi_slave_transaction_t*, void*)>;

void spi_slave_post_setup_cb(spi_slave_transaction_t* trans);
void spi_slave_post_trans_cb(spi_slave_transaction_t* trans);
struct spi_slave_context_t
{
    spi_slave_interface_config_t if_cfg {
        .spics_io_num = SS,
        .flags = 0,
        .queue_size = 3, // Increased default queue size
        .mode = SPI_MODE0,
        .post_setup_cb = nullptr, // Default to nullptr
        .post_trans_cb = nullptr, // Default to nullptr
    };
    spi_bus_config_t bus_cfg {
        .mosi_io_num = MOSI,
        .miso_io_num = MISO,
        .sclk_io_num = SCK,
        .data2_io_num = -1,
        .data3_io_num = -1,
        .data4_io_num = -1,
        .data5_io_num = -1,
        .data6_io_num = -1,
        .data7_io_num = -1,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 1)
        .data_io_default_level = false,
#endif
        .max_transfer_sz = SOC_SPI_MAXIMUM_BUFFER_SIZE,
        .flags = SPICOMMON_BUSFLAG_SLAVE,
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 2, 0)
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO,
#elif ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        .isr_cpu_id = INTR_CPU_ID_AUTO,
#endif
        .intr_flags = 0,
    };
    spi_host_device_t host {SPI2_HOST};
    TaskHandle_t main_task_handle {NULL};
};

struct spi_transaction_context_t
{
    spi_slave_transaction_t *trans;
    size_t size;
    TickType_t timeout_ticks;
};

struct spi_slave_cb_user_context_t
{
    struct {
        spi_slave_user_cb_t user_cb;
        void *user_arg;
    } post_setup;
    struct {
        spi_slave_user_cb_t user_cb;
        void *user_arg;
    } post_trans;
};

void IRAM_ATTR spi_slave_post_setup_cb(spi_slave_transaction_t* trans)
{
    spi_slave_cb_user_context_t *user_ctx = static_cast<spi_slave_cb_user_context_t*>(trans->user);
    if (user_ctx->post_setup.user_cb) {
        user_ctx->post_setup.user_cb(trans, user_ctx->post_setup.user_arg);
    }
}

void IRAM_ATTR spi_slave_post_trans_cb(spi_slave_transaction_t* trans)
{
    spi_slave_cb_user_context_t *user_ctx = static_cast<spi_slave_cb_user_context_t*>(trans->user);
    if (user_ctx->post_trans.user_cb) {
        user_ctx->post_trans.user_cb(trans, user_ctx->post_trans.user_arg);
    }
}

void spi_slave_task(void *arg)
{
    ESP_LOGD(TAG, "spi_slave_task start");

    spi_slave_context_t *ctx = static_cast<spi_slave_context_t*>(arg);

    esp_err_t err = spi_slave_initialize(ctx->host, &ctx->bus_cfg, &ctx->if_cfg, SPI_DMA_DISABLED);
    assert(err == ESP_OK);

    s_trans_queue_handle = xQueueCreate(3, sizeof(spi_transaction_context_t));
    assert(s_trans_queue_handle != NULL);
    s_trans_result_handle = xQueueCreate(ctx->if_cfg.queue_size, sizeof(size_t));
    assert(s_trans_result_handle != NULL);
    s_trans_error_handle = xQueueCreate(ctx->if_cfg.queue_size, sizeof(esp_err_t));
    assert(s_trans_error_handle != NULL);
    s_in_flight_mailbox_handle = xQueueCreate(1, sizeof(size_t));
    assert(s_in_flight_mailbox_handle != NULL);

    while (true) {
        spi_transaction_context_t trans_ctx;
        if (xQueueReceive(s_trans_queue_handle, &trans_ctx, RECV_TRANS_QUEUE_TIMEOUT_TICKS)) {
            assert(trans_ctx.trans != nullptr);
            assert(trans_ctx.size <= ctx->if_cfg.queue_size);
            xQueueOverwrite(s_in_flight_mailbox_handle, &trans_ctx.size);

            ESP_LOGD(TAG, "new transaction request received (size = %u)", trans_ctx.size);
            std::vector<esp_err_t> errs;
            errs.reserve(trans_ctx.size);
            for (size_t i = 0; i < trans_ctx.size; ++i) {
                spi_slave_transaction_t *trans = &trans_ctx.trans[i];
                esp_err_t err = spi_slave_queue_trans(ctx->host, trans, trans_ctx.timeout_ticks);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "failed to execute spi_slave_queue_trans(): 0x%X", err);
                }
                errs.push_back(err);
            }

            xQueueReset(s_trans_result_handle);
            xQueueReset(s_trans_error_handle);
            for (size_t i = 0; i < trans_ctx.size; ++i) {
                size_t num_received_bytes = 0;
                if (errs[i] == ESP_OK) {
                    spi_slave_transaction_t *rtrans;
                    esp_err_t err = spi_slave_get_trans_result(ctx->host, &rtrans, trans_ctx.timeout_ticks);
                    if (err != ESP_OK) {
                        ESP_LOGE(TAG, "failed to execute spi_slave_get_trans_result(): 0x%X", err);
                    } else {
                        num_received_bytes = rtrans->trans_len / 8;
                        ESP_LOGD(TAG, "transaction complete: %d bits (%d bytes) received", rtrans->trans_len, num_received_bytes);
                    }
                } else {
                    ESP_LOGE(TAG, "skip spi_slave_get_trans_result() because queue was failed: index = %u", i);
                }

                if (!xQueueSend(s_trans_result_handle, &num_received_bytes, SEND_TRANS_RESULT_TIMEOUT_TICKS)) {
                    ESP_LOGE(TAG, "failed to send a number of received bytes to main task: %d", err);
                }
                if (!xQueueSend(s_trans_error_handle, &errs[i], SEND_TRANS_ERROR_TIMEOUT_TICKS)) {
                    ESP_LOGE(TAG, "failed to send a transaction error to main task: %d", err);
                }

                const size_t num_rest_in_flight = trans_ctx.size - (i + 1);
                xQueueOverwrite(s_in_flight_mailbox_handle, &num_rest_in_flight);
            }

            delete[] trans_ctx.trans;
            ESP_LOGD(TAG, "all requested transactions completed");
        }

        if (xTaskNotifyWait(0, 0, NULL, 0) == pdTRUE) {
            break;
        }
    }

    ESP_LOGD(TAG, "terminate spi task as requested by the main task");

    vQueueDelete(s_in_flight_mailbox_handle);
    vQueueDelete(s_trans_result_handle);
    vQueueDelete(s_trans_error_handle);
    vQueueDelete(s_trans_queue_handle);

    spi_slave_free(ctx->host);

    xTaskNotifyGive(ctx->main_task_handle);
    ESP_LOGD(TAG, "spi_slave_task finished");

    vTaskDelete(NULL);
}

class Slave
{
    spi_slave_context_t ctx;
    std::vector<spi_slave_transaction_t> transactions;
    spi_slave_cb_user_context_t cb_user_ctx;
    TaskHandle_t spi_task_handle {NULL};

public:
    bool begin(const uint8_t spi_bus = VSPI) // Default to VSPI
    {
#ifdef CONFIG_IDF_TARGET_ESP32
        this->ctx.if_cfg.spics_io_num = (spi_bus == VSPI) ? 5 : 15;
        this->ctx.bus_cfg.sclk_io_num = (spi_bus == VSPI) ? 18 : 14;
        this->ctx.bus_cfg.mosi_io_num = (spi_bus == VSPI) ? 23 : 13;
        this->ctx.bus_cfg.miso_io_num = (spi_bus == VSPI) ? 19 : 12;
#endif
        return this->initialize(spi_bus);
    }

    bool begin(uint8_t spi_bus, int sck, int miso, int mosi, int ss)
    {
        this->ctx.if_cfg.spics_io_num = ss;
        this->ctx.bus_cfg.sclk_io_num = sck;
        this->ctx.bus_cfg.mosi_io_num = mosi;
        this->ctx.bus_cfg.miso_io_num = miso;
        return this->initialize(spi_bus);
    }

    bool begin(uint8_t spi_bus, int sck, int ss, int data0, int data1, int data2, int data3)
    {
        this->ctx.if_cfg.spics_io_num = ss;
        this->ctx.bus_cfg.sclk_io_num = sck;
        this->ctx.bus_cfg.data0_io_num = data0;
        this->ctx.bus_cfg.data1_io_num = data1;
        this->ctx.bus_cfg.data2_io_num = data2;
        this->ctx.bus_cfg.data3_io_num = data3;
        return this->initialize(spi_bus);
    }

    bool begin(uint8_t spi_bus, int sck, int ss, int data0, int data1, int data2, int data3, int data4, int data5, int data6, int data7)
    {
        this->ctx.if_cfg.spics_io_num = ss;
        this->ctx.bus_cfg.sclk_io_num = sck;
        this->ctx.bus_cfg.data0_io_num = data0;
        this->ctx.bus_cfg.data1_io_num = data1;
        this->ctx.bus_cfg.data2_io_num = data2;
        this->ctx.bus_cfg.data3_io_num = data3;
        this->ctx.bus_cfg.data4_io_num = data4;
        this->ctx.bus_cfg.data5_io_num = data5;
        this->ctx.bus_cfg.data6_io_num = data6;
        this->ctx.bus_cfg.data7_io_num = data7;
        return this->initialize(spi_bus);
    }

    void end()
    {
        if (this->spi_task_handle == NULL) {
            ESP_LOGW(TAG, "spi_slave_task already terminated");
            return;
        }
        xTaskNotifyGive(this->spi_task_handle);
        if (xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(5000)) != pdTRUE) {
            ESP_LOGW(TAG, "timeout waiting for the termination of spi_slave_task");
        }
        this->spi_task_handle = NULL;
    }

    size_t transfer(const uint8_t* tx_buf, uint8_t* rx_buf, size_t size, uint32_t timeout_ms = 0)
    {
        return this->transfer(0, tx_buf, rx_buf, size, timeout_ms);
    }

    size_t transfer(uint32_t flags, const uint8_t* tx_buf, uint8_t* rx_buf, size_t size, uint32_t timeout_ms)
    {
        if (!this->queue(flags, tx_buf, rx_buf, size)) {
            return 0;
        }
        const auto results = this->wait(timeout_ms);
        if (results.empty()) {
            return 0;
        } else {
            return results[results.size() - 1];
        }
    }

    bool queue(const uint8_t* tx_buf, uint8_t* rx_buf, size_t size)
    {
        return this->queue(0, tx_buf, rx_buf, size);
    }

    bool queue(uint32_t flags, const uint8_t* tx_buf, uint8_t* rx_buf, size_t size)
    {
        if (this->transactions.size() >= this->ctx.if_cfg.queue_size) {
            ESP_LOGW(TAG, "failed to queue transaction: queue is full - only %u transactions can be queued at once", this->ctx.if_cfg.queue_size);
            return false;
        }
        this->queueTransaction(flags, size, tx_buf, rx_buf);
        return true;
    }

    std::vector<size_t> wait(uint8_t* rx_buf, const uint8_t* tx_buf, size_t size, uint32_t timeout_ms = 1000) // Added new wait method
    {
        if (!this->queue(tx_buf, rx_buf, size)) {
            return std::vector<size_t>();
        }
        return this->wait(timeout_ms);
    }

    std::vector<size_t> wait(uint32_t timeout_ms = 1000)
    {
        size_t num_will_be_queued = this->transactions.size();
        if (!this->trigger(timeout_ms)) {
            return std::vector<size_t>();
        }
        return this->waitTransaction(num_will_be_queued);
    }

    bool trigger(uint32_t timeout_ms = 1000)
    {
        if (this->transactions.empty()) {
            ESP_LOGW(TAG, "failed to trigger transaction: no transaction is queued");
            return false;
        }
        if (this->numTransactionsInFlight() > 0) {
            ESP_LOGW(TAG, "failed to trigger transaction: there are already in-flight transactions");
            return false;
        }

        spi_transaction_context_t trans_ctx {
            .trans = new spi_slave_transaction_t[this->transactions.size()],
            .size = this->transactions.size(),
            .timeout_ticks = timeout_ms == 0 ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms),
        };
        for (size_t i = 0; i < this->transactions.size(); i++) {
            trans_ctx.trans[i] = std::move(this->transactions[i]);
        }
        int ret = xQueueSend(s_trans_queue_handle, &trans_ctx, SEND_TRANS_QUEUE_TIMEOUT_TICKS);
        this->transactions.clear();
        if (!ret) {
            ESP_LOGE(TAG, "failed to queue transaction: transaction queue between main and spi task is full");
            return false;
        }
        return true;
    }

    size_t available()
    {
        return this->numTransactionsCompleted();
    }

    void pop()
    {
        if (this->numTransactionsCompleted() > 0) {
            this->numBytesReceived();
        }
    }

    size_t numTransactionsInFlight()
    {
        size_t num_in_flight = 0;
        xQueuePeek(s_in_flight_mailbox_handle, &num_in_flight, 0);
        return num_in_flight;
    }

    size_t numTransactionsCompleted()
    {
        return uxQueueMessagesWaiting(s_trans_result_handle);
    }

    size_t numTransactionErrors()
    {
        return uxQueueMessagesWaiting(s_trans_error_handle);
    }

    size_t numBytesReceived()
    {
        if (this->numTransactionsCompleted() > 0) {
            size_t num_received_bytes = 0;
            if (xQueueReceive(s_trans_result_handle, &num_received_bytes, RECV_TRANS_RESULT_TIMEOUT_TICKS)) {
                return num_received_bytes;
            } else {
                ESP_LOGE(TAG, "failed to receive queued result");
                return 0;
            }
        }
        return 0;
    }

    std::vector<size_t> numBytesReceivedAll()
    {
        std::vector<size_t> results;
        const size_t num_results = this->numTransactionsCompleted();
        results.reserve(num_results);
        for (size_t i = 0; i < num_results; ++i) {
            results.emplace_back(this->numBytesReceived());
        }
        return results;
    }

    esp_err_t error()
    {
        if (this->numTransactionErrors() > 0) {
            esp_err_t err;
            if (xQueueReceive(s_trans_error_handle, &err, RECV_TRANS_ERROR_TIMEOUT_TICKS)) {
                return err;
            } else {
                ESP_LOGE(TAG, "failed to receive queued error");
                return ESP_FAIL;
            }
        }
        return ESP_OK;
    }

    std::vector<esp_err_t> errors()
    {
        std::vector<esp_err_t> errs;
        const size_t num_errs = this->numTransactionErrors();
        errs.reserve(num_errs);
        for (size_t i = 0; i < num_errs; ++i) {
            errs.emplace_back(this->error());
        }
        return errs;
    }

    bool hasTransactionsCompletedAndAllResultsHandled()
    {
        return this->numTransactionsInFlight() == 0 && this->numTransactionsCompleted() == 0;
    }

    bool hasTransactionsCompletedAndAllResultsReady(size_t num_queued)
    {
        return this->numTransactionsInFlight() == 0 && this->numTransactionsCompleted() == num_queued;
    }

    void setDataMode(uint8_t mode)
    {
        this->setSpiMode(mode);
    }

    void setQueueSize(size_t size)
    {
        this->ctx.if_cfg.queue_size = size;
    }

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 1)
    void setDataIODefaultLevel(bool level)
    {
        this->ctx.bus_cfg.data_io_default_level = level;
    }
#endif

    void setSlaveFlags(uint32_t flags) { this->ctx.if_cfg.flags = flags; }
    void setSpiMode(uint8_t m) { this->ctx.if_cfg.mode = m; }
    void setPostSetupCb(const slave_transaction_cb_t &post_setup_cb) { this->ctx.if_cfg.post_setup_cb = post_setup_cb; }
    void setPostTransCb(const slave_transaction_cb_t &post_trans_cb) { this->ctx.if_cfg.post_trans_cb = post_trans_cb; }
    void setUserPostSetupCbAndArg(const spi_slave_user_cb_t &cb, void *arg)
    {
        this->cb_user_ctx.post_setup.user_cb = cb;
        this->cb_user_ctx.post_setup.user_arg = arg;
    }
    void setUserPostTransCbAndArg(const spi_slave_user_cb_t &cb, void *arg)
    {
        this->cb_user_ctx.post_trans.user_cb = cb;
        this->cb_user_ctx.post_trans.user_arg = arg;
    }

private:
    static spi_host_device_t hostFromBusNumber(uint8_t spi_bus)
    {
        switch (spi_bus) {
            case FSPI:
#ifdef CONFIG_IDF_TARGET_ESP32
                return SPI1_HOST;
#else
                return SPI2_HOST;
#endif
            case HSPI:
#if defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32C3)
                return SPI2_HOST;
#else
                return SPI3_HOST;
#endif
#ifdef CONFIG_IDF_TARGET_ESP32
            case VSPI:
                return SPI3_HOST;
#endif
            default:
                return SPI2_HOST;
        }
    }

    bool initialize(const uint8_t spi_bus)
    {
        this->ctx.host = this->hostFromBusNumber(spi_bus);
        this->ctx.bus_cfg.flags |= SPICOMMON_BUSFLAG_SLAVE;
        this->ctx.main_task_handle = xTaskGetCurrentTaskHandle();
        this->transactions.reserve(this->ctx.if_cfg.queue_size);

        std::string task_name = std::string("spi_slave_task_") + std::to_string(this->ctx.if_cfg.spics_io_num);
#if SOC_CPU_CORES_NUM == 1
        int ret = xTaskCreatePinnedToCore(spi_slave_task, task_name.c_str(), SPI_SLAVE_TASK_STASCK_SIZE, static_cast<void*>(&this->ctx), SPI_SLAVE_TASK_PRIORITY, &this->spi_task_handle, 0);
#else
        int ret = xTaskCreatePinnedToCore(spi_slave_task, task_name.c_str(), SPI_SLAVE_TASK_STASCK_SIZE, static_cast<void*>(&this->ctx), SPI_SLAVE_TASK_PRIORITY, &this->spi_task_handle, 1);
#endif
        if (ret != pdPASS) {
            ESP_LOGE(TAG, "failed to create spi_slave_task: %d", ret);
            return false;
        }

        return true;
    }

    spi_slave_transaction_t generateTransaction(uint32_t flags, size_t size, const uint8_t* tx_buf, uint8_t* rx_buf)
    {
        spi_slave_transaction_t trans;
        trans.length = 8 * size;
        trans.trans_len = 0;
        trans.tx_buffer = (tx_buf == nullptr) ? NULL : tx_buf;
        trans.rx_buffer = (rx_buf == nullptr) ? NULL : rx_buf;
        trans.user = &this->cb_user_ctx;
        return trans;
    }

    void queueTransaction(uint32_t flags, size_t size, const uint8_t* tx_buf, uint8_t* rx_buf)
    {
        spi_slave_transaction_t trans = generateTransaction(flags, size, tx_buf, rx_buf);
        this->transactions.push_back(std::move(trans));
    }

    std::vector<size_t> waitTransaction(size_t num_will_be_queued)
    {
        while (!this->hasTransactionsCompletedAndAllResultsReady(num_will_be_queued)) {
            vTaskDelay(1);
        }
        return this->numBytesReceivedAll();
    }
};

ARDUINO_ESP32_SPI_SLAVE_NAMESPACE_END

using ESP32SPISlave = arduino::esp32::spi::slave::Slave;

#endif
