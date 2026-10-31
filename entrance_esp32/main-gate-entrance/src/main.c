#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/ledc.h"

#define GREEN_LED GPIO_NUM_32
#define RED_LED   GPIO_NUM_33
#define BUZZER    GPIO_NUM_27

#define ENTRY_SERVO GPIO_NUM_25
#define EXIT_SERVO  GPIO_NUM_26

#define UART_PORT UART_NUM_0

typedef struct {
    char cardID[16];
    char name[30];
    char role[20];
    bool inside;
} User;

User users[] = {
    {"COCIS-001", "Dr. Ritah",      "LECTURER", false},
    {"COCIS-002", "John Student",   "STUDENT",  false},
    {"COCIS-003", "System Admin",   "ADMIN",    false}
};

int occupancy = 0;

uint32_t servoDuty(uint32_t us)
{
    return (us * 65535) / 20000;
}

void setServo(ledc_channel_t channel, uint32_t pulse)
{
    ledc_set_duty(
        LEDC_LOW_SPEED_MODE,
        channel,
        servoDuty(pulse));

    ledc_update_duty(
        LEDC_LOW_SPEED_MODE,
        channel);
}

void openEntryGate()
{
    printf("ENTRY GATE OPENING...\n");

    setServo(LEDC_CHANNEL_0, 2000);

    vTaskDelay(pdMS_TO_TICKS(2000));

    setServo(LEDC_CHANNEL_0, 1000);

    printf("ENTRY GATE CLOSED\n");
}

void openExitGate()
{
    printf("EXIT GATE OPENING...\n");

    setServo(LEDC_CHANNEL_1, 2000);

    vTaskDelay(pdMS_TO_TICKS(2000));

    setServo(LEDC_CHANNEL_1, 1000);

    printf("EXIT GATE CLOSED\n");
}

void greenFlash()
{
    gpio_set_level(GREEN_LED, 1);

    vTaskDelay(pdMS_TO_TICKS(1500));

    gpio_set_level(GREEN_LED, 0);
}

void redFlash()
{
    gpio_set_level(RED_LED, 1);

    vTaskDelay(pdMS_TO_TICKS(1500));

    gpio_set_level(RED_LED, 0);
}

void beepOnce()
{
    gpio_set_level(BUZZER,1);

    vTaskDelay(pdMS_TO_TICKS(200));

    gpio_set_level(BUZZER,0);
}

void beepDenied()
{
    for(int i=0;i<3;i++)
    {
        gpio_set_level(BUZZER,1);

        vTaskDelay(pdMS_TO_TICKS(200));

        gpio_set_level(BUZZER,0);

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

int getUser(char *id)
{
    for(int i=0;i<3;i++)
    {
        if(strcmp(users[i].cardID,id)==0)
            return i;
    }

    return -1;
}

void grantEntry(char *card)
{
    int user = getUser(card);

    if(user < 0)
    {
        printf("\nCARD NOT REGISTERED\n");
        printf("ACCESS DENIED\n");

        redFlash();
        beepDenied();

        return;
    }

    if(users[user].inside)
    {
        printf("\n%s already inside building\n",
               users[user].name);

        printf("ACCESS DENIED\n");

        redFlash();
        beepDenied();

        return;
    }

    users[user].inside = true;

    occupancy++;

    printf("\nCARD VERIFIED\n");

    printf("Name : %s\n",
           users[user].name);

    printf("Role : %s\n",
           users[user].role);

    printf("ACCESS GRANTED\n");

    printf("Current Occupancy = %d\n",
           occupancy);

    greenFlash();

    beepOnce();

    openEntryGate();
}

void grantExit(char *card)
{
    int user = getUser(card);

    if(user < 0)
    {
        printf("\nCARD NOT REGISTERED\n");
        printf("EXIT DENIED\n");

        redFlash();
        beepDenied();

        return;
    }

    if(!users[user].inside)
    {
        printf("\nUSER NOT INSIDE BUILDING\n");
        printf("EXIT DENIED\n");

        redFlash();
        beepDenied();

        return;
    }

    users[user].inside = false;

    occupancy--;

    printf("\nEXIT VERIFIED\n");

    printf("Name : %s\n",
           users[user].name);

    printf("Role : %s\n",
           users[user].role);

    printf("EXIT GRANTED\n");

    printf("Current Occupancy = %d\n",
           occupancy);

    greenFlash();

    beepOnce();

    openExitGate();
}

void showUsers()
{
    printf("\n============== INSIDE BUILDING ==============\n");

    for(int i=0;i<3;i++)
    {
        if(users[i].inside)
        {
            printf("%s (%s)\n",
                   users[i].name,
                   users[i].role);
        }
    }
}

void menu()
{
    printf("\n");
    printf("============================================\n");
    printf(" CoCIS MAIN GATE CONTROL SYSTEM\n");
    printf("============================================\n");
    printf("1. Lecturer Entry\n");
    printf("2. Student Entry\n");
    printf("3. Admin Entry\n");
    printf("4. Unknown Entry\n");
    printf("5. Lecturer Exit\n");
    printf("6. Student Exit\n");
    printf("7. Admin Exit\n");
    printf("8. Show Occupancy\n");
    printf("9. Show Users Inside\n");
    printf("============================================\n");
    printf("Select Option: ");
    fflush(stdout);
}

void serialTask(void *arg)
{
    uint8_t buffer[64];

    menu();

    while(1)
    {
        int len = uart_read_bytes(
            UART_PORT,
            buffer,
            sizeof(buffer)-1,
            pdMS_TO_TICKS(100));

        if(len > 0)
        {
            buffer[len] = '\0';

            int option = atoi((char*)buffer);

            switch(option)
            {
                case 1:
                    grantEntry("COCIS-001");
                    break;

                case 2:
                    grantEntry("COCIS-002");
                    break;

                case 3:
                    grantEntry("COCIS-003");
                    break;

                case 4:
                    grantEntry("COCIS-999");
                    break;

                case 5:
                    grantExit("COCIS-001");
                    break;

                case 6:
                    grantExit("COCIS-002");
                    break;

                case 7:
                    grantExit("COCIS-003");
                    break;

                case 8:
                    printf("\nCurrent Occupancy = %d\n",
                           occupancy);
                    break;

                case 9:
                    showUsers();
                    break;
            }

            menu();
        }
    }
}

void app_main()
{
    gpio_reset_pin(GREEN_LED);
    gpio_reset_pin(RED_LED);
    gpio_reset_pin(BUZZER);

    gpio_set_direction(GREEN_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(RED_LED, GPIO_MODE_OUTPUT);
    gpio_set_direction(BUZZER, GPIO_MODE_OUTPUT);

    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_16_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK
    };

    ledc_timer_config(&timer);

    ledc_channel_config_t entryServo = {
        .gpio_num = ENTRY_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0
    };

    ledc_channel_config_t exitServo = {
        .gpio_num = EXIT_SERVO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0
    };

    ledc_channel_config(&entryServo);
    ledc_channel_config(&exitServo);

    setServo(LEDC_CHANNEL_0, 1000);
    setServo(LEDC_CHANNEL_1, 1000);

    uart_driver_install(UART_NUM_0,
                        1024,
                        0,
                        0,
                        NULL,
                        0);

    printf("\nMAIN GATE SYSTEM READY\n");

    xTaskCreate(
        serialTask,
        "serialTask",
        4096,
        NULL,
        5,
        NULL);

    while(1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
} 