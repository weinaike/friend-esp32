#include "wifi_board.h"
#include "audio_codecs/es8311_audio_codec.h"
#include "display/display.h"
#include "application.h"
#include "button.h"
#include "led/single_led.h"
#include "iot/thing_manager.h"
#include "settings.h"
#include "config.h"
#include "power_save_timer.h"
#include "font_awesome_symbols.h"
#include "assets/lang_config.h"
#include <wifi_station.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>
#include <driver/rtc_io.h>
#include "esp_sleep.h"

	
#define GPIO_WAKEUP_NUM BOOT_BUTTON_GPIO 
#define GPIO_WAKEUP_LEVEL 0
	
#define ESP_RETURN_ON_ERROR(x, log_tag, format, ...) do {                                   \
		esp_err_t err_rc_ = (x);																\
		if (unlikely(err_rc_ != ESP_OK)) {														\
			ESP_LOGE(log_tag, "%s(%d): " format, __FUNCTION__, __LINE__, ##__VA_ARGS__);		\
			return err_rc_; 																	\
		}																						\
	} while(0)

#define TAG "FriendC3Board"

class FriendC3Board : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_;
    Display* display_ = nullptr;
    Button boot_button_;

    PowerSaveTimer* power_save_timer_ = nullptr;
    
    // 标记是否从深度睡眠唤醒,以及启动时间戳
    bool woke_from_deep_sleep_ = false;

    void InitializePowerSaveTimer() {

        power_save_timer_ = new PowerSaveTimer(-1, 120, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling light sleep mode");            

        });
        power_save_timer_->OnExitSleepMode([this]() {
            ESP_LOGI(TAG, "Exit light sleep mode");    
        });
        
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutdown request received, cleaning up resources before deep sleep");
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateIdle);
            vTaskDelay(pdMS_TO_TICKS(100));
            app.Alert(Lang::Strings::BATTERY_SLEEP, Lang::Strings::BATTERY_SLEEP, "neutral", Lang::Sounds::P3_SLEEP);
            // Wait for audio to finish playing
            app.WaitForAudioPlayback();
            

            auto codec = GetAudioCodec();
            if (codec) {
                ESP_LOGI(TAG, "Disabling audio codec");
                codec->EnableInput(false);
                codec->EnableOutput(false);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            // 3. 配置GPIO唤醒源
            _register_gpio_wakeup();
            
            // 4. 进入深度睡眠
            ESP_LOGI(TAG, "Entering deep sleep mode");
            esp_deep_sleep_start();
        });

        power_save_timer_->SetEnabled(true);
    }

    void InitializeCodecI2c() {
        // Initialize I2C peripheral
        i2c_master_bus_config_t i2c_bus_cfg = {
            .i2c_port = I2C_NUM_0,
            .sda_io_num = AUDIO_CODEC_I2C_SDA_PIN,
            .scl_io_num = AUDIO_CODEC_I2C_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }
    void InitializeDisplay() {
        if (display_) {
            ESP_LOGW(TAG, "Display already exists, cleaning up first");
            delete display_;
            display_ = nullptr;
        }
        display_ = new NoDisplay();
        ESP_LOGI(TAG, "Display initialized");
    }

	esp_err_t _register_gpio_wakeup(void)
	{

        // const int wakeup_time_sec = 20;
        // ESP_LOGI(TAG, "Enabling timer wakeup, %ds\n", wakeup_time_sec);
        // ESP_ERROR_CHECK(esp_sleep_enable_timer_wakeup(wakeup_time_sec * 1000000));

		/* Initialize GPIO */
		const gpio_config_t config = {
				.pin_bit_mask = BIT64(GPIO_WAKEUP_NUM),
				.mode = GPIO_MODE_INPUT,
				.pull_up_en = GPIO_PULLUP_ENABLE,  // 启用内部上拉
				.pull_down_en = GPIO_PULLDOWN_DISABLE,
				.intr_type = GPIO_INTR_DISABLE
		};
		ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "Initialize GPIO%d failed", GPIO_WAKEUP_NUM);
	
		/* Enable wake up from GPIO for deep sleep */
		ESP_RETURN_ON_ERROR(esp_deep_sleep_enable_gpio_wakeup(BIT64(GPIO_WAKEUP_NUM), ESP_GPIO_WAKEUP_GPIO_LOW),
							TAG, "Enable gpio deep sleep wakeup failed");
	
		ESP_LOGI(TAG, "gpio deep sleep wakeup source is ready");
	
		return ESP_OK;
	}

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                    // 检测是否是深度睡眠唤醒后的误触发. 如果是从深度睡眠唤醒,忽略首次点击
                    if (woke_from_deep_sleep_) {
                        ESP_LOGI(TAG, "Ignoring first click after wake from deep sleep");                        
                    }
                    else {
                        ESP_LOGI(TAG, "Entering WiFi configuration mode");
                        ResetWifiConfiguration();
                    }
            }
            woke_from_deep_sleep_ = false;
            app.ToggleChatState();
        });

        // 修正：长按直接进入深度休眠，唤醒后自动复位，无需恢复外设
        boot_button_.OnLongPressUp([this]() {
            ESP_LOGW(TAG, "Key button long press released, cleaning up and entering deep sleep mode");
            auto& app = Application::GetInstance();
            app.SetDeviceState(kDeviceStateIdle);
            vTaskDelay(pdMS_TO_TICKS(100));
            app.Alert(Lang::Strings::BATTERY_SLEEP, Lang::Strings::BATTERY_SLEEP, "neutral", Lang::Sounds::P3_SLEEP);
            // Wait for audio to finish playing
            app.WaitForAudioPlayback();
            
            // 1. 停止音频编解码器
            auto codec = GetAudioCodec();
            if (codec) {
                ESP_LOGI(TAG, "Disabling audio codec");
                codec->EnableInput(false);
                codec->EnableOutput(false);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
            // 3. ESP32-C3 使用 GPIO 唤醒方式进入深度睡眠
            _register_gpio_wakeup();
            esp_deep_sleep_start();
        });

        // 三击直接进入配网
        boot_button_.OnMultipleClick([this]() {  
            ESP_LOGI(TAG, "Key button triple clicked, entering WiFi configuration mode");
            auto& app = Application::GetInstance();
            // 如果处于配网模式，则重启；如果其他模式，则进入配网模式
            if (app.GetDeviceState() == kDeviceStateWifiConfiguring) {
                app.Reboot();
            } else {
                ResetWifiConfiguration();
            }
            
        }, 3); // 三击配网
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        Settings settings("vendor");
#if CONFIG_IOT_PROTOCOL_XIAOZHI        
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
#endif  
    }

public:
    FriendC3Board() : boot_button_(BOOT_BUTTON_GPIO) {  
		ESP_LOGI(TAG, "enter FriendC3Board  construct func");
     
        // 检测是否从深度睡眠唤醒
        esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
        if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
            woke_from_deep_sleep_ = true;
            ESP_LOGI(TAG, "Woke up from deep sleep by GPIO");
        }
        
        // 把 ESP32C3 的 VDD SPI 引脚作为普通 GPIO 口使用
        esp_efuse_write_field_bit(ESP_EFUSE_VDD_SPI_AS_GPIO);

        InitializeCodecI2c();
        InitializeDisplay();
        InitializeButtons();
        InitializePowerSaveTimer();
        InitializeIot();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK, AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR);
        return &audio_codec;
    }
};

DECLARE_BOARD(FriendC3Board);
