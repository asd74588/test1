#include "ota_state_machine.h"



// 状态转移
void state_machine_transition(StateMachine* sm, State new_state) {
    if (new_state >= OTA_COUNT) return;
    
    sm->previous_state = sm->current_state;
    sm->current_state = new_state;
    sm->handler = state_handlers[new_state];
    
    printf("[State Machine] %s -> %s\n", 
           state_names[sm->previous_state],
           state_names[sm->current_state]);
}

// 初始化
void state_machine_init(StateMachine* sm, void* context) {
    memset(sm, 0, sizeof(StateMachine));
    sm->context = context;
    sm->current_state = OTA_STATE_BOOT;
    sm->handler = boot_state_handler;
}

// 事件分发（事件驱动核心）
void state_machine_dispatch(StateMachine* sm, EventData* event) {
    if (!sm || !sm->handler || !event) return;
    
    printf("\n[Event] %s received in state %s\n", 
           event_names[event->type], 
           state_names[sm->current_state]);
    
    // 调用当前状态的处理函数
    sm->handler(sm, event);
}

// 简化的事件分发（不带数据）
void state_machine_send_event(StateMachine* sm, Event event) {
    EventData evt = {
        .type = event,
        .data = NULL,
        .data_size = 0
    };
    state_machine_dispatch(sm, &evt);
}



void boot_state_handler(StateMachine* sm, EventData* event)
{
    switch (event->type)
    {
        case EV_TICK:
                printf("  Action: Processing tick...\n");
                // 执行周期性任务
                break;
        case EV_STOP:
            printf("  Action: Stopping gracefully...\n");
            state_machine_transition(sm, ST_STOPPED);
            break;
        case EV_START:
                /* code */
                break;
        default:
            break;
    }
}


void upgeading_state_handler(StateMachine* sm, EventData* event);
{

}


void verifying_state_handler(StateMachine* sm, EventData* event);
{

}


void revert_state_handler(StateMachine* sm, EventData* event);
{

}

