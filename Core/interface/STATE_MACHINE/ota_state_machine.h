#ifndef __OTA_STATE_MACHINE_H
#define __OTA_STATE_MACHINE_H


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "global.h"

// ========== 基础定义 ==========
typedef enum {
    OTA_STATE_BOOT,
    OTA_STATE_UPGRADING,
    OTA_STATE_VERIFYING,
    OTA_STATE_REVERT,
    OTA_COUNT
} State;

typedef enum {
    EV_START,
    EV_STOP,
    EV_TICK,
    EV_ERROR_OCCUR,
    EV_ERROR_CLEAR,
    EV_DATA_RECEIVED,
    EV_TIMEOUT,
    EV_COUNT
} Event;

// 事件数据（可携带额外信息）
typedef struct {
    Event type;
    void* data;
    int data_size;
} EventData;



// 状态机结构
typedef struct StateMachine StateMachine;
// 状态处理函数类型（接收事件）
typedef void (*StateHandler)(StateMachine* sm, EventData* event);

struct StateMachine {
    State current_state;
    State previous_state;
    StateHandler handler;
    void* context;
    
    // 事件队列（可选）
    EventData* event_queue;
    int queue_size;
    int queue_head;
    int queue_tail;
    int queue_capacity;
};


// 状态处理函数声明
void boot_state_handler(StateMachine* sm, EventData* event);
void upgeading_state_handler(StateMachine* sm, EventData* event);
void verifying_state_handler(StateMachine* sm, EventData* event);
void revert_state_handler(StateMachine* sm, EventData* event);

// 状态函数表
static const StateHandler state_handlers[OTA_COUNT] = {
    boot_state_handler,
    upgeading_state_handler,
    verifying_state_handler,
    revert_state_handler
};

// 状态名称
static const char* state_names[OTA_COUNT] = {
    "OTA_STATE_BOOT", "OTA_STATE_UPGRADING",
    "OTA_STATE_VERIFYING", "OTA_STATE_REVERT"
};

//事件名称
static const char* event_names[EV_COUNT] = {
    "START", "STOP", "TICK", "ERROR_OCCUR", 
    "ERROR_CLEAR", "DATA_RECEIVED", "TIMEOUT"
};



#endif

