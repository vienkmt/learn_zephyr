/*
 * Bốn thread Zephyr trên STM32F411E-DISCO.
 * Cam và xanh lá nháy đều. Đỏ nháy nhịp tim.
 * Thread nút: bấm SW0 tăng độ sáng LED xanh dương thêm 10%.
 *
 * Board do Zephyr khai báo sẵn. Project này chỉ chọn board lúc build
 * (west build -b stm32f411e_disco) rồi dùng tên chung led0, led1.
 * Đổi kit thì đổi -b. Code gọi led0 vẫn giữ nguyên, chân thật nằm ở file board mới.
 *
 * Ba lớp:
 *   Chip:  zephyr/dts/arm/st/f4/stm32f4.dtsi
 *          Khối phần cứng của STM32F4. Cổng D là node gpiod.
 *   Board: zephyr/boards/st/stm32f411e_disco/stm32f411e_disco.dts
 *          Gắn chân chip vào LED và nút trên mạch này.
 *          led0 = LD3 cam PD13, led1 = LD4 xanh lá PD12,
 *          led2 = LD5 đỏ PD14, led3 = LD6 xanh dương PD15.
 *          stm32f411e_disco_defconfig bật driver sẵn cho kit.
 *   App:   src/main.c và prj.conf trong thư mục dự án này.
 *
 * LED lấy từ devicetree của board, không ghi số chân trong code:
 *   led0 = LD3 cam,  chân PD13
 *   led1 = LD4 xanh lá, chân PD12
 *
 * Cam và xanh lá nháy đều, dùng chung blink_thread().
 * Đỏ chạy heartbeat_thread(): hai nhịp ngắn rồi nghỉ dài.
 * button_thread() đọc SW0. Mỗi lần bấm tăng PWM led3 thêm 10%.
 *
 * Bốn thread cùng mức ưu tiên. Thread đang chạy chỉ nhường CPU khi gọi
 * k_msleep(). Lúc đó bộ lập lịch chạy thread khác.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/sys/printk.h>

/* Mỗi thread có stack riêng. 1024 byte đủ cho hàm nháy LED này. */
#define STACK_SIZE 1024

/*
 * Mức ưu tiên. Số càng nhỏ càng được chạy trước.
 * Bốn thread cùng số 5 nên không cái nào chen ngang cái kia khi đang chạy.
 */
#define PRIORITY 5

/* Nháy đều: đảo LED rồi ngủ một khoảng cố định. */
struct blink_cfg {
	const struct gpio_dt_spec *led; /* LED lấy từ devicetree */
	const char *name;               /* Tên thread, để phân biệt khi debug */
	int32_t period_ms;              /* Thời gian giữa hai lần đảo LED */
};

/*
 * Nhịp tim: bật, tắt, bật, rồi nghỉ dài.
 * on_ms  = thời gian mỗi lần sáng.
 * gap_ms = thời gian tắt giữa hai lần bật liên tiếp.
 * rest_ms = thời gian nghỉ giữa hai lần nháy.
 */
struct heartbeat_cfg {
	const struct gpio_dt_spec *led;
	const char *name;
	int32_t on_ms;
	int32_t gap_ms;
	int32_t rest_ms;
};

/*
 * GPIO_DT_SPEC_GET đọc chân, cổng và mức tích cực từ alias trong file .dts
 * của board. Đổi board thì alias led0, led1 trỏ sang chân khác, code không sửa.
 */
static const struct gpio_dt_spec orange_led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec green_led = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec red_led = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);
static const struct gpio_dt_spec user_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);

/* pwm-led3 trong file board là LD6 xanh dương, TIM4 kênh 4, chân PD15. */
static const struct pwm_dt_spec blue_pwm = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led3));

/* Khoảng đọc nút. Vừa nhường CPU, vừa lọc nhiễu tiếp xúc khi bấm. */
#define BUTTON_POLL_MS 20
#define BLUE_STEP_PERCENT 10
#define BLUE_MAX_PERCENT 90
/* 1 kHz: mắt thấy sáng đều, không nháy theo từng xung. */
#define BLUE_PERIOD_NS PWM_USEC(1000)

static const struct blink_cfg orange_cfg = {
	.led = &orange_led,
	.name = "orange",
	.period_ms = 500,
};

static const struct blink_cfg green_cfg = {
	.led = &green_led,
	.name = "green",
	.period_ms = 1500,
};

static const struct heartbeat_cfg red_cfg = {
	.led = &red_led,
	.name = "red",
	.on_ms = 100,
	.gap_ms = 120,
	.rest_ms = 700,
};

/* Bật chân GPIO và kiểm tra driver. Trả về 0 khi sẵn sàng. */
static int led_prepare(const struct gpio_dt_spec *led, const char *name)
{
	if (!gpio_is_ready_dt(led)) {
		printk("%s: GPIO not ready\n", name);
		return -1;
	}

	if (gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE) < 0) {
		printk("%s: pin configure failed\n", name);
		return -1;
	}

	return 0;
}

/* Sáng một nhịp rồi tắt. k_msleep nhường CPU trong lúc LED đang sáng. */
static void led_pulse(const struct gpio_dt_spec *led, int32_t on_ms)
{
	gpio_pin_set_dt(led, 1);
	k_msleep(on_ms);
	gpio_pin_set_dt(led, 0);
}

/*
 * Hàm của một thread Zephyr luôn có dạng (p1, p2, p3).
 * Ở đây p1 là con trỏ tới blink_cfg. p2 và p3 không dùng.
 */
static void blink_thread(void *p1, void *p2, void *p3)
{
	const struct blink_cfg *cfg = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Đặt tên thread. Cần CONFIG_THREAD_NAME=y trong prj.conf. */
	k_thread_name_set(k_current_get(), cfg->name);

	if (led_prepare(cfg->led, cfg->name) < 0) {
		return;
	}

	printk("%s thread started, toggle every %d ms\n", cfg->name, cfg->period_ms);

	while (1) {
		gpio_pin_toggle_dt(cfg->led); /* Sáng thành tắt, hoặc tắt thành sáng. */
		k_msleep(cfg->period_ms);     /* Ngủ và nhường CPU cho thread khác. */
	}
}

/* LED đỏ: hai nhịp ngắn, nghỉ dài, rồi lặp. */
static void heartbeat_thread(void *p1, void *p2, void *p3)
{
	const struct heartbeat_cfg *cfg = p1;

	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), cfg->name);

	if (led_prepare(cfg->led, cfg->name) < 0) {
		return;
	}

	printk("%s heartbeat on %d ms, gap %d ms, rest %d ms\n",
	       cfg->name, cfg->on_ms, cfg->gap_ms, cfg->rest_ms);

	while (1) {
		led_pulse(cfg->led, cfg->on_ms); /* Nhịp thứ nhất. */
		k_msleep(cfg->gap_ms);           /* Tắt giữa hai lần bật. */
		led_pulse(cfg->led, cfg->on_ms); /* Nhịp thứ hai. */
		k_msleep(cfg->rest_ms);          /* Nghỉ trước lần nháy sau. */
	}
}

/* Đặt độ sáng LED xanh. 0 là tắt. pulse là phần chu kỳ PWM ở mức cao. */
static int blue_set_percent(uint8_t percent)
{
	uint32_t pulse = (BLUE_PERIOD_NS * percent) / 100;

	return pwm_set_dt(&blue_pwm, BLUE_PERIOD_NS, pulse);
}

/*
 * Bấm SW0 (nút User, chân PA0) tăng sáng LD6 thêm 10%.
 * Mặc định tắt. Vượt 90% thì quay về 0. Giữ nút không tăng thêm.
 */
static void button_thread(void *p1, void *p2, void *p3)
{
	int was_pressed = 0;
	uint8_t percent = 0;

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	k_thread_name_set(k_current_get(), "button");

	if (!pwm_is_ready_dt(&blue_pwm)) {
		printk("blue pwm not ready\n");
		return;
	}

	if (blue_set_percent(0) < 0) {
		printk("blue pwm set failed\n");
		return;
	}

	if (!gpio_is_ready_dt(&user_button)) {
		printk("sw0: GPIO not ready\n");
		return;
	}

	if (gpio_pin_configure_dt(&user_button, GPIO_INPUT) < 0) {
		printk("sw0: pin configure failed\n");
		return;
	}

	printk("button thread: sw0 steps blue brightness\n");

	while (1) {
		int pressed = gpio_pin_get_dt(&user_button);

		if (pressed && !was_pressed) {
			percent += BLUE_STEP_PERCENT;
			if (percent > BLUE_MAX_PERCENT) {
				percent = 0;
			}

			if (blue_set_percent(percent) < 0) {
				printk("blue pwm set %u failed\n", percent);
			} else {
				printk("blue brightness %u%%\n", percent);
			}
		}

		was_pressed = pressed;
		k_msleep(BUTTON_POLL_MS);
	}
}

/*
 * K_THREAD_DEFINE tạo thread ngay từ lúc biên dịch.
 * Kernel tự chạy thread này khi boot, không cần gọi trong main().
 *
 * Tham số lần lượt:
 *   tên thread, kích thước stack, hàm chạy,
 *   p1, p2, p3,
 *   mức ưu tiên, tùy chọn (0 = không thêm gì),
 *   số ms chờ trước khi được chạy (0 = chạy ngay).
 */
K_THREAD_DEFINE(orange_tid, STACK_SIZE, blink_thread,
		&orange_cfg, NULL, NULL, PRIORITY, 0, 0);
		
K_THREAD_DEFINE(green_tid, STACK_SIZE, blink_thread,
		&green_cfg, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(red_tid, STACK_SIZE, heartbeat_thread,
		&red_cfg, NULL, NULL, PRIORITY, 0, 0);

K_THREAD_DEFINE(button_tid, STACK_SIZE, button_thread,
		NULL, NULL, NULL, PRIORITY, 0, 0);

/*
 * main() cũng là một thread. Nó chỉ in một dòng rồi kết thúc.
 * return 0 không tắt máy. Bốn thread kia vẫn chạy vòng lặp riêng.
 * Kit này không có cổng serial USB nên dòng printk không hiện trên máy Mac.
 */
int main(void)
{
	printk("STM32F411E-DISCO: blink + heartbeat + pwm button\n");
	printk("sw0 steps blue PWM by 10%%, max 90, then back to off\n");
	return 0;
}
