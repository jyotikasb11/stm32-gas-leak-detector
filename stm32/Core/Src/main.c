/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Gas leak detection + wireless logging via ESP32/ThingSpeak
  *                   (TIM2 hardware timer interrupt + EXTI13 button interrupt)
  ******************************************************************************
  */
/* USER CODE END Header */
#include "main.h"

/* USER CODE BEGIN Includes */
#include "i2c_lcd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* USER CODE END Includes */

/* USER CODE BEGIN PD */
#define SAMPLE_COUNT      50
#define CALIB_DELAY_MS    100

#define STEEP_DELTA        100    // how far above baseline counts as a leak
#define RECOVERY_MARGIN     50
#define STABLE_MARGIN       50
#define STABLE_TIME_MS   20000

#define SEND_INTERVAL_MS  1000    // how often we send a reading to the ESP32

#define BUZZER_ON()   (GPIOB->BSRR = GPIO_PIN_3)
#define BUZZER_OFF()  (GPIOB->BSRR = (uint32_t)GPIO_PIN_3 << 16)

// Fan is wired to the relay's NC terminal (NO contact on this module is worn).
// NC conducts when the relay is de-energized, so the fan logic is inverted:
// de-energize the relay to turn the fan ON, energize it to turn the fan OFF.
#define FAN_ON()      (GPIOB->BSRR = (uint32_t)GPIO_PIN_5 << 16)
#define FAN_OFF()     (GPIOB->BSRR = GPIO_PIN_5)

/* ---- Hardware timer (TIM2) : sampling period ---------------------------
 * TIM2 clock = 16 MHz (HSI, APB1 prescaler = 1 -> timer clock = PCLK1).
 * PSC = 15999 -> 1 kHz counter, ARR = 299 -> update event every 300 ms.   */
#define SAMPLE_PERIOD_MS  300
#define BUTTON_DEBOUNCE_MS 200
/* USER CODE END PD */

/* USER CODE BEGIN PV */
static volatile uint8_t sampleDue    = 0;   // set by TIM2 interrupt every 300 ms
static volatile uint8_t recalRequest = 0;   // set by EXTI13 (B1 button) interrupt
/* USER CODE END PV */

ADC_HandleTypeDef hadc1;
I2C_HandleTypeDef hi2c1;
UART_HandleTypeDef huart1;

void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_ADC1_Init(void);
static void MX_I2C1_Init(void);
static void MX_USART1_UART_Init(void);
static void MX_TIM2_Init(void);
static void MX_EXTI13_Init(void);

/* USER CODE BEGIN 4 */
uint16_t Read_MQ6(void)
{
    ADC1->CR2 |= ADC_CR2_SWSTART;
    while (!(ADC1->SR & ADC_SR_EOC));
    return (uint16_t)(ADC1->DR);
}

uint16_t Calibrate_Baseline(void)
{
    uint32_t sum = 0;
    for (int i = 0; i < SAMPLE_COUNT; i++) {
        sum += Read_MQ6();
        HAL_Delay(CALIB_DELAY_MS);
    }
    return (uint16_t)(sum / SAMPLE_COUNT);
}

void Send_To_ESP32(uint16_t gasValue, const char *status, uint16_t baseline, int32_t delta, uint8_t buzzerState)
{
    char txBuf[48];
    int len = snprintf(txBuf, sizeof(txBuf), "%d,%s,%d,%ld,%d\n",
                        gasValue, status, baseline, (long)delta, buzzerState);
    HAL_UART_Transmit(&huart1, (uint8_t*)txBuf, len, 100);
}

/* ---------- TIM2 : 300 ms periodic update interrupt (register level) ---- */
static void MX_TIM2_Init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN;
    TIM2->CR1  = 0;
    TIM2->PSC  = 16000 - 1;               // 16 MHz / 16000 = 1 kHz
    TIM2->ARR  = SAMPLE_PERIOD_MS - 1;    // 300 counts = 300 ms
    TIM2->EGR  = TIM_EGR_UG;              // load PSC/ARR
    TIM2->SR   = 0;                       // clear the update flag set by UG
    TIM2->DIER |= TIM_DIER_UIE;           // enable update interrupt
    NVIC_SetPriority(TIM2_IRQn, 1);
    NVIC_EnableIRQ(TIM2_IRQn);
}

/* ---------- EXTI13 : Nucleo user button B1 (PC13, active low) ----------- */
static void MX_EXTI13_Init(void)
{
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    GPIOC->MODER &= ~(3U << (13 * 2));                    // PC13 = input
    SYSCFG->EXTICR[3] &= ~(0xFU << 4);                    // EXTI13 <- port C
    SYSCFG->EXTICR[3] |=  (0x2U << 4);
    EXTI->FTSR |= EXTI_FTSR_TR13;                         // falling edge (button press)
    EXTI->RTSR &= ~EXTI_RTSR_TR13;
    EXTI->PR    = EXTI_PR_PR13;                           // clear any pending flag
    EXTI->IMR  |= EXTI_IMR_MR13;                          // unmask line 13
    NVIC_SetPriority(EXTI15_10_IRQn, 2);
    NVIC_EnableIRQ(EXTI15_10_IRQn);
}

/* ---------- Interrupt service routines ---------------------------------- */
void TIM2_IRQHandler(void)
{
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR = ~TIM_SR_UIF;           // clear update flag
        sampleDue = 1;
    }
}

void EXTI15_10_IRQHandler(void)
{
    static uint32_t lastPress = 0;
    if (EXTI->PR & EXTI_PR_PR13) {
        EXTI->PR = EXTI_PR_PR13;          // clear pending bit (write 1)
        uint32_t t = HAL_GetTick();
        if ((t - lastPress) > BUTTON_DEBOUNCE_MS) {
            lastPress = t;
            recalRequest = 1;             // manual recalibration request
        }
    }
}
/* USER CODE END 4 */

int main(void)
{
  HAL_Init();
  SystemClock_Config();

  MX_GPIO_Init();
  MX_ADC1_Init();
  MX_I2C1_Init();
  MX_USART1_UART_Init();
  MX_TIM2_Init();
  MX_EXTI13_Init();

  /* USER CODE BEGIN 2 */
  FAN_OFF();
  BUZZER_OFF();

  ADC1->CR2 |= ADC_CR2_ADON;
  HAL_Delay(1);

  lcd_init(&hi2c1);
  lcd_clear();
  lcd_put_cur(0, 0);
  lcd_send_string("Gas Monitor Init");
  HAL_Delay(1000);

  lcd_clear();
  lcd_put_cur(0, 0);
  lcd_send_string("Calibrating...");
  uint16_t baseline = Calibrate_Baseline();

  char lineBuf[17];
  lcd_clear();
  lcd_put_cur(0, 0);
  snprintf(lineBuf, sizeof(lineBuf), "Base: %4d", baseline);
  lcd_send_string(lineBuf);
  HAL_Delay(1500);
  lcd_clear();

  uint8_t  leakActive     = 0;
  uint16_t stableRefValue = 0;
  uint32_t stableStartTick = 0;
  uint32_t lastSendTick = HAL_GetTick();

  sampleDue = 0;
  recalRequest = 0;
  TIM2->CR1 |= TIM_CR1_CEN;               // start the 300 ms hardware timer
  /* USER CODE END 2 */

  while (1)
  {
    /* USER CODE BEGIN WHILE */
    // Manual recalibration requested by the B1 button interrupt (same routine
    // as the automatic plateau recalibration; no effect unless button pressed)
    if (recalRequest) {
        recalRequest = 0;
        leakActive = 0;
        BUZZER_OFF();
        FAN_OFF();
        lcd_clear();
        lcd_put_cur(0, 0);
        lcd_send_string("Recalibrating..");
        baseline = Calibrate_Baseline();
        lcd_clear();
    }

    uint16_t gasValue = Read_MQ6();
    uint32_t now = HAL_GetTick();
    const char *statusText;
    int32_t delta = (int32_t)gasValue - (int32_t)baseline;

    lcd_put_cur(0, 0);
    snprintf(lineBuf, sizeof(lineBuf), "Gas: %4d       ", gasValue);
    lcd_send_string(lineBuf);

    if (!leakActive) {
        if (delta > STEEP_DELTA) {
            leakActive = 1;
            BUZZER_ON();
            FAN_ON();
            stableRefValue  = gasValue;
            stableStartTick = now;
        }
        statusText = "Safe";
        lcd_put_cur(1, 0);
        lcd_send_string("Safe.  Fan: OFF");

    } else {
        if (gasValue <= (baseline + RECOVERY_MARGIN)) {
            leakActive = 0;
            BUZZER_OFF();
            FAN_OFF();
            statusText = "Safe";
            lcd_put_cur(1, 0);
            lcd_send_string("Safe.  Fan: OFF");
        } else {
            int32_t drift = (int32_t)gasValue - (int32_t)stableRefValue;
            if (abs((int)drift) > STABLE_MARGIN) {
                stableRefValue  = gasValue;
                stableStartTick = now;
            }

            if ((now - stableStartTick) >= STABLE_TIME_MS) {
                leakActive = 0;
                BUZZER_OFF();
                FAN_OFF();

                lcd_clear();
                lcd_put_cur(0, 0);
                lcd_send_string("Recalibrating..");
                baseline = Calibrate_Baseline();
                lcd_clear();
                statusText = "Safe";
            } else {
                statusText = "LEAK";
                lcd_put_cur(1, 0);
                lcd_send_string("LEAK! Fan: ON  ");
            }
        }
    }

    if ((now - lastSendTick) >= SEND_INTERVAL_MS) {
        Send_To_ESP32(gasValue, statusText, baseline, delta, leakActive);
        lastSendTick = now;
    }

    // Sleep until the TIM2 interrupt signals the next 300 ms sample slot
    // (replaces HAL_Delay(300); SysTick still wakes the core every 1 ms)
    while (!sampleDue) {
        __WFI();
    }
    sampleDue = 0;
    /* USER CODE END WHILE */
  }
}

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE2);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = 1;
  sConfig.SamplingTime = ADC_SAMPLETIME_3CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_I2C1_Init(void)
{
  hi2c1.Instance = I2C1;
  hi2c1.Init.ClockSpeed = 100000;
  hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_USART1_UART_Init(void)
{
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 9600;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
}

static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_3|GPIO_PIN_5, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = GPIO_PIN_3|GPIO_PIN_5;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

void Error_Handler(void)
{
  __disable_irq();
  while (1)
  {
  }
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
}
#endif