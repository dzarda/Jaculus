#pragma once

#include <freertos/task.h>
#include <freertos/timers.h>
#include <jsmachine.hpp>

extern "C" {
    extern const uint8_t rtosTimerWrappersStart[]
        asm("_binary_rtosTimerWrappers_js_start");
    extern const uint8_t rtosTimerWrappersEnd[]
        asm("_binary_rtosTimerWrappers_js_end");
}


namespace jac {

// Implement timers functionality for the JsMachine.
//
// Internally keeps a list of timer structures. The corresponding callback for
// the timer is stored in <stash>.timerSlot[String(id)].
template < typename Self >
class RtosTimers {
    static inline constexpr const char* SLOT = "timerSlot";
public:
    MACHINE_FEATURE_SELF();

    struct Configuration {};

    void initialize() {
        setupSlot();
        registerFunctions();
        registerRuntime();

        m_startTicks = xTaskGetTickCount();
    }

    void onEventLoop() {}
private:
    void setupSlot() {
        duk_push_heap_stash( self()._context );
        duk_push_object( self()._context );
        duk_put_prop_string( self()._context, -2, SLOT );
        duk_pop( self()._context );
    }

    void registerFunctions() {
        duk_push_c_function( self()._context, dukCreateTimer, 3 );
        duk_put_global_string( self()._context, "createTimer" );

        duk_push_c_function( self()._context, dukDeleteTimer, 1 );
        duk_put_global_string( self()._context, "deleteTimer" );

        duk_push_c_function( self()._context, dukMillis, 0 );
        duk_put_global_string( self()._context, "millis" );
    }

    void registerRuntime() {
        duk_context* ctx = self()._context;
        duk_push_lstring( ctx, reinterpret_cast< const char * >( rtosTimerWrappersStart ),
            rtosTimerWrappersEnd - rtosTimerWrappersStart );
        duk_push_string( ctx, "/builtin/rtosTimerWrappers.js" );
        duk_compile( ctx, DUK_COMPILE_EVAL );
	    duk_call( ctx, 0 );
        duk_pop( ctx );
    }

    TimerHandle_t createTimer( int period, bool oneShot ) {
        if ( period == 0 ) {
            throw std::runtime_error( "Timers with no period are not implemented yet" );
        }
        if ( oneShot )
            return xTimerCreate(
                nullptr, pdMS_TO_TICKS( period ),
                !oneShot, &self(), timerCallback< false > );
        else
            return xTimerCreate(
                nullptr, pdMS_TO_TICKS( period ),
                !oneShot, &self(), timerCallback< true > );
    }

    template < bool AutoReload >
    static void timerCallback( TimerHandle_t timer ) {
        int timerId = reinterpret_cast< int >( timer );
        auto& self = Self::fromUdata( pvTimerGetTimerID( timer ) );

        self.schedule( [&]( duk_context* ctx ) {
            duk_push_c_function( ctx, dukInvokeTimer, 2 );
            duk_push_int( ctx, timerId );
            duk_push_boolean( ctx, !AutoReload );
        } );

        if ( !AutoReload ) {
            xTimerDelete( timer, portMAX_DELAY );
        }
    }

    // Accepts the following duk arguments:
    // - period: number - period in milliseconds
    // - oneShot: bool  - declare if the timer is one shot or not
    // - callback: fun  - timer callback
    static duk_ret_t dukCreateTimer( duk_context* ctx ) {
        Self& self = Self::fromContext( ctx );

        // Validate arguments
        int period = duk_require_number( ctx, 0 );
        bool oneShot = duk_require_boolean( ctx, 1 );
        duk_require_function( ctx, 2 );

        TimerHandle_t t = self.createTimer( period, oneShot );
        xTimerStart( t, portMAX_DELAY );
        int timerId = reinterpret_cast< int >( t );

        duk_push_heap_stash( ctx );
        duk_get_prop_string( ctx, -1, SLOT );
        auto slotOffset = duk_get_top_index( ctx );

        duk_dup( ctx, 2 );
        duk_put_prop_index( ctx, slotOffset, timerId );

        return dukReturn( ctx, timerId );
    }

    // Accepts the following duk arguments:
    // - timer: number - timer identifier
    // - cleanup: bool - declare if the timer callback should be cleaned or not
    static duk_ret_t dukInvokeTimer( duk_context* ctx ) {
        // Extract time callback
        duk_push_heap_stash( ctx );
        duk_get_prop_string( ctx, -1, SLOT );
        auto slotOffset = duk_get_top_index( ctx );
        duk_dup( ctx, 0 );
        duk_get_prop( ctx, slotOffset );

        // Invoke callback
        duk_require_callable( ctx, -1 );
        duk_call( ctx, 0 );

        if ( duk_require_boolean( ctx, 1 ) ) {
            duk_dup( ctx, 0 );
            duk_del_prop( ctx, slotOffset );
        }
        return 0;
    }

    // Accepts the following duk arguments:
    // - timer: number - timer identifier
    // Returns nothing.
    static duk_ret_t dukDeleteTimer(duk_context* ctx) {
        int timerId = duk_require_int(ctx, 0);

        // Delete time callback
        duk_push_heap_stash( ctx );
        duk_get_prop_string( ctx, -1, SLOT );
        auto slotOffset = duk_get_top_index( ctx );
        duk_dup( ctx, 0 );
        if(duk_has_prop(ctx, slotOffset)) {
            duk_dup( ctx, 0 );
            duk_del_prop( ctx, slotOffset );

            // Delete FreeRTOS timer
            xTimerDelete(reinterpret_cast<TimerHandle_t>(timerId), portMAX_DELAY);
        }
        return 0;
    }

    // No arguments.
    // Returns number of millis since jacMachine start.
    static duk_ret_t dukMillis(duk_context* ctx) {
        Self& self = Self::fromContext( ctx );
        auto ticks = xTaskGetTickCount() - self.m_startTicks;
        return dukReturn(ctx, (int)(ticks * portTICK_RATE_MS));
    }

    TickType_t m_startTicks;
};

} // namespace jac
