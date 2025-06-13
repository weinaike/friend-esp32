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

#include <wifi_station.h>
#include <esp_log.h>
#include <esp_efuse_table.h>
#include <driver/i2c_master.h>

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
    // bool press_to_talk_enabled_ = false; // 删除 按下说话模式 与 点击说话模式 的区分
    PowerSaveTimer* power_save_timer_ = nullptr;

    void InitializePowerSaveTimer() {
        power_save_timer_ = new PowerSaveTimer(160, 60);
        power_save_timer_->OnEnterSleepMode([this]() {
            ESP_LOGI(TAG, "Enabling sleep mode");            
            _register_gpio_wakeup();
			_enter_light_sleep();

        });
        power_save_timer_->OnExitSleepMode([this]() {
            auto codec = GetAudioCodec();
            codec->EnableInput(true);     
            codec->SetOutputVolume(50); // 恢复音量
            _revert_from_sleep();       
        });
        power_save_timer_->SetEnabled(true);

        // 关闭 这个逻辑对吗？
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutdown request received");
            _register_gpio_wakeup();
			_enter_light_sleep();
        });


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
    void _deinitCodecI2c(void) {
    }
    void InitializeDisplay() {
        if (display_) {
            ESP_LOGW(TAG, "Display already initialized, skipping.");
            return;
        }
        display_ = new NoDisplay(); 
    }

    void _deinitDisplay(void) {
        if (display_) {
            delete display_;
            display_ = nullptr;
        }
    }
	void _wait_wakeup_gpio_inactive(void)
	{
		printf("Waiting for GPIO%d to go high...\n", GPIO_WAKEUP_NUM);
		while (gpio_get_level(GPIO_WAKEUP_NUM) == GPIO_WAKEUP_LEVEL) {
			vTaskDelay(pdMS_TO_TICKS(10));
		}
	}
	esp_err_t _register_gpio_wakeup(void)
	{
		/* Initialize GPIO */
		gpio_config_t config = {
				.pin_bit_mask = BIT64(GPIO_WAKEUP_NUM),
				.mode = GPIO_MODE_INPUT,
				.pull_up_en = GPIO_PULLUP_DISABLE,
				.pull_down_en = GPIO_PULLDOWN_DISABLE,
				.intr_type = GPIO_INTR_DISABLE
		};
		ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "Initialize GPIO%d failed", GPIO_WAKEUP_NUM);
	
		/* Enable wake up from GPIO */
		ESP_RETURN_ON_ERROR(gpio_wakeup_enable(GPIO_WAKEUP_NUM, GPIO_INTR_LOW_LEVEL),
							TAG, "Enable gpio wakeup failed");
		ESP_RETURN_ON_ERROR(esp_sleep_enable_gpio_wakeup(), TAG, "Configure gpio as wakeup source failed");
	
		/* Make sure the GPIO is inactive and it won't trigger wakeup immediately */
		_wait_wakeup_gpio_inactive();
		ESP_LOGI(TAG, "gpio wakeup source is ready");
	
		return ESP_OK;
	}
    void _enter_light_sleep(void)
    {
        auto codec = GetAudioCodec();
        codec->EnableInput(false);
        //gpio_hold_dis(MCU_VCC_CTL);
        //gpio_set_level(MCU_VCC_CTL, 0);
        _deinitCodecI2c();
        _deinitDisplay();
        /* Enter sleep mode */
        //esp_light_sleep_start();

        /* Determine wake up reason */
        esp_sleep_wakeup_cause_t wakeup_reason =esp_sleep_get_wakeup_cause();
        if (wakeup_reason == ESP_SLEEP_WAKEUP_GPIO) {
            /* Waiting for the gpio inactive, or the chip will continuously trigger wakeup*/
            _wait_wakeup_gpio_inactive();
        }
        else{
            ESP_LOGI(TAG, "wakeup source is  %d", wakeup_reason);
        }

        if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO) {
            /* Waiting for the gpio inactive, or the chip will continuously trigger wakeup*/
            _wait_wakeup_gpio_inactive();
        }
    }
    void _revert_from_sleep(void)
    {
        InitializeCodecI2c();
        InitializeDisplay();
        InitializeIot();
    }
	

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        boot_button_.OnLongPress([this]() {
			int64_t now = esp_timer_get_time();
			ESP_LOGW(TAG, "Key button long pressed,  now: %lld", now);
			_register_gpio_wakeup();
			_enter_light_sleep();
            _revert_from_sleep();
        });

        // 三击直接进入配网
        boot_button_.OnMultipleClick([this]() {                       
            ResetWifiConfiguration();
        }, 3); // 三击配网
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        Settings settings("vendor");
        auto& thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
    }

public:
    FriendC3Board() : boot_button_(BOOT_BUTTON_GPIO) {  
		ESP_LOGI(TAG, "enter FriendC3Board  construct func");
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
