/* Подключаем заголовок, который сгенерировал CubeMX
   В нём описаны типы HAL, прототипы SystemClock_Config и т.п. */
#include "main.h"
/* Стандартные заголовки для printf/strlen */
#include <stdio.h>
#include <string.h>

/* ==== Глобальные объекты периферии ==== */

/* Дескриптор таймера TIM2 — нужен для delay_us() */
TIM_HandleTypeDef htim2;
/* Дескриптор UART2 — для вывода строк в терминал */
UART_HandleTypeDef huart2;

/* Прототипы функций инициализации, которые мы определим ниже */
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART2_UART_Init(void);

/* ==== Пользовательская часть 0: функции HX711 и UART ==== */

/* Микросекундная задержка на TIM2.
   Предполагаем, что TIM2 настроен так, что 1 тик = 1 мкс.
   Устанавливаем счётчик в 0 и ждём, пока он не дорастёт до us. */
static void delay_us(uint16_t us)
{
  __HAL_TIM_SET_COUNTER(&htim2, 0);
  while (__HAL_TIM_GET_COUNTER(&htim2) < us);
}

/* Чтение "сырых" данных HX711.
   Используем: DT/DOUT на PB7 (вход), SCK/CLK на PA15 (выход).
   Алгоритм повторяет библиотеку HX711 для Arduino:
   - ждём, пока DOUT станет LOW (данные готовы)
   - генерируем 24 такта SCK, на каждом читаем один бит
   - даём 25‑й такт для выбора канала/усиления
   - расширяем знак 24‑битного значения до 32‑битного */
static int32_t HX711_ReadRaw(void)
{
  uint32_t data = 0;
  /* Таймаут на случай, если датчик "умрёт" и DOUT не опустится */
  uint32_t timeout = HAL_GetTick();

  /* 1. Ждём, пока линия DOUT (PB7) станет LOW.
        Это означает "данные готовы". */
  while (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_SET)
  {
    if ((HAL_GetTick() - timeout) > 500)  // 500 мс
      return 0;                           // вернём 0 при таймауте
  }

  /* 2. Читаем 24 бита.
        Каждый цикл:
        - поднимаем SCK (PA15)
        - ждём пару микросекунд
        - сдвигаем накопленное значение влево
        - опускаем SCK
        - снова ждём
        - читаем состояние DOUT и, если там 1, ставим младший бит */
  for (int i = 0; i < 24; i++)
  {
    /* фронт SCK = 1 */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);
    delay_us(2);   // ширина импульса ~2 мкс

    /* готовим место под следующий бит */
    data <<= 1;

    /* спад SCK = 0 */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_RESET);
    delay_us(2);   // пауза между тактами

    /* читаем DOUT (PB7); если HIGH, ставим младший бит в 1 */
    if (HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_7) == GPIO_PIN_SET)
      data |= 1;
  }

  /* 3. Делаем 25‑й импульс CLOCK.
        Он говорит HX711: "принято, следующую выборку давай с тем же усилением". */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_SET);
  delay_us(2);
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_RESET);
  delay_us(2);

  /* 4. Расширяем знак.
        HX711 выдаёт 24‑битное значение в дополнительном коде (signed).
        Старший бит (bit 23) — знак. Если он 1, нужно "дозаполнить" старшие
        биты единицами, чтобы получить корректное 32‑битное signed. */
  if (data & 0x800000)      // если установлен бит 23
  {
    data |= 0xFF000000;     // забиваем старшие 8 бит единицами
  }

  /* Теперь data — корректный int32 с тем же смыслом, что на Arduino. */
  return (int32_t)data;
}

/* Удобная обёртка для вывода C‑строки через UART2 */
static void uart_print(const char *s)
{
  /* Блокирующая передача: ждём до 100 мс */
  HAL_UART_Transmit(&huart2, (uint8_t*)s, strlen(s), 100);
}

/* ==== main ==== */

/**
  * @brief  Точка входа программы.
  */
int main(void)
{
  /* Инициализация HAL: включает системные таймеры, NVIC и базовые вещи */
  HAL_Init();
  /* Настройка тактирования (HSI, делители шин и т.п.) */
  SystemClock_Config();

  /* Инициализация GPIO (PA15, PB7), таймера TIM2 и UART2 */
  MX_GPIO_Init();
  MX_TIM2_Init();
  MX_USART2_UART_Init();

  /* Запускаем TIM2, чтобы delay_us() мог работать */
  HAL_TIM_Base_Start(&htim2);

  char buf[64];      // буфер для строки printf
  int32_t tare = 0;  // будущая "тара" — среднее значение без нагрузки

  uart_print("HX711 start\r\n");

  /* Первые 10 измерений используются для тарировки:
     берём 10 сырых значений, усредняем и запоминаем как "0". */
  for (int i = 0; i < 10; i++)
  {
    tare += HX711_ReadRaw();
    HAL_Delay(50);          // небольшая пауза между измерениями
  }
  tare /= 10;               // среднее из 10 значений

  uart_print("Tare done\r\n");

  /* Основной цикл: читаем датчик, вычитаем тару, печатаем raw и net */
  while (1)
  {
    int32_t raw = HX711_ReadRaw();   // сырое значение от АЦП
    int32_t net = raw - tare;        // "чистое" значение после тары

    /* Формируем строку вида: raw=-528270, net=-52285 */
    snprintf(buf, sizeof(buf), "raw=%ld, net=%ld\r\n",
             (long)raw, (long)net);
    uart_print(buf);

    HAL_Delay(300); // обновляем показания примерно 3–4 раза в секунду
  }
}

/* ==== Ниже — служебные функции, сгенерированные и слегка правленные ==== */

/**
  * @brief Настройка системного тактирования.
  * Здесь включаем HSI (16 МГц) и настраиваем делители шин.
  * Для нашего примера достаточно базовой конфигурации CubeMX.
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK
                              | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1
                              | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Настройка TIM2.
  * Делаем так, чтобы таймер тикал с частотой 1 МГц (1 мкс на тик):
  * HSI = 16 МГц, Prescaler = 16-1 → 16 МГц / 16 = 1 МГц.
  */
static void MX_TIM2_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  htim2.Instance = TIM2;
  htim2.Init.Prescaler         = 16 - 1;          // 16 МГц / 16 = 1 МГц
  htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
  htim2.Init.Period            = 0xFFFF;          // максимум 65535 мкс
  htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Настройка USART2 (TX/RX) для вывода в терминал.
  * Стандартные параметры: 115200 бод, 8N1, без аппаратного контроля потока.
  */
static void MX_USART2_UART_Init(void)
{
  huart2.Instance        = USART2;
  huart2.Init.BaudRate   = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits   = UART_STOPBITS_1;
  huart2.Init.Parity     = UART_PARITY_NONE;
  huart2.Init.Mode       = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl  = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief Настройка GPIO.
  * Включаем тактирование портов A и B.
  * PA15 — SCK (выход без подтяжек).
  * PB7  — DOUT/DT (вход без подтяжек).
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* Начальное состояние PA15 = 0 (SCK в LOW) */
  HAL_GPIO_WritePin(GPIOA, GPIO_PIN_15, GPIO_PIN_RESET);

  /* SCK: PA15 как Output Push-Pull, без подтяжек, низкая скорость */
  GPIO_InitStruct.Pin   = GPIO_PIN_15;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* DT: PB7 как вход без подтяжек */
  GPIO_InitStruct.Pin  = GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
}

/**
  * @brief Обработчик ошибки: просто встаём в бесконечный цикл.
  */
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
  /* Можно добавить вывод имени файла и строки */
}
#endif /* USE_FULL_ASSERT */
