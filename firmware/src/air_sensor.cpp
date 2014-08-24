/*
 * Copyright (c) 2014 Courierdrone, courierdrone.com
 * Distributed under the MIT License, available in the file LICENSE.
 * Author: Pavel Kirienko <pavel.kirienko@courierdrone.com>
 */

#include "air_sensor.hpp"
#include "board/ms5611.h"
#include "node.hpp"

#include <uavcan/equipment/air_data/StaticAirData.hpp>

#include <ch.hpp>
#include <crdr_chibios/sys/sys.h>
#include <crdr_chibios/config/config.hpp>
#include <crdr_chibios/watchdog/watchdog.hpp>
#include <unistd.h>

namespace air_sensor
{
namespace
{

const float ValidPressureRange[] = { 1000, 120000 };
const float ValidTemperatureRange[] = { -40, 85 };

crdr_chibios::config::Param<bool> param_enabled("air_data_enabled", true);
crdr_chibios::config::Param<float> param_pressure_variance("pressure_variance_pa2", 100.0, 1.0, 4000.0);
crdr_chibios::config::Param<float> param_temperature_variance("temperature_variance_degc2", 4.0, 1.0, 100.0);

class AirSensorThread : public chibios_rt::BaseStaticThread<1024>
{
    const unsigned PeriodUSec = 100000;

    float pressure_variance = 0;
    float temperature_variance = 0;

    mutable crdr_chibios::watchdog::Timer watchdog_;
    mutable ::Ms5611 sens = ::Ms5611();

    static bool isInRange(float value, const float range_min_max[2])
    {
        return (value >= range_min_max[0]) && (value <= range_min_max[1]);
    }

    void publish(float pressure_pa, float temperature_degc) const
    {
        if (!node::isStarted())
        {
            return;
        }

        static uavcan::equipment::air_data::StaticAirData air_data;
        air_data.static_pressure = pressure_pa;
        air_data.static_pressure_variance = pressure_variance;

        air_data.static_temperature = temperature_degc;
        air_data.static_temperature_variance = temperature_variance;

        node::Lock locker;
        auto& node = node::getNode();

        static uavcan::Publisher<uavcan::equipment::air_data::StaticAirData> air_data_pub(node);
        (void)air_data_pub.broadcast(air_data);
    }

    void tryRun() const
    {
        systime_t sleep_until = chibios_rt::System::getTime();
        while (true)
        {
            watchdog_.reset();
            sleep_until += US2ST(PeriodUSec);

            int32_t raw_pressure = 0;
            int32_t raw_temperature = 0;
            if (!ms5611ReadPT(&sens, &raw_pressure, &raw_temperature))
            {
                break;
            }

            const float pressure = static_cast<float>(raw_pressure);
            const float temperature = raw_temperature / 100.F;

            publish(pressure, temperature);

            if (!isInRange(pressure, ValidPressureRange) ||
                !isInRange(temperature, ValidTemperatureRange))
            {
                node::setComponentStatus(node::ComponentID::AirSensor, uavcan::protocol::NodeStatus::STATUS_CRITICAL);
            }
            else
            {
                node::setComponentStatus(node::ComponentID::AirSensor, uavcan::protocol::NodeStatus::STATUS_OK);
            }

            chibios_rt::BaseThread::sleepUntil(sleep_until);
        }
    }

public:
    msg_t main() override
    {
        watchdog_.startMSec(1000);

        pressure_variance = param_pressure_variance.get();
        temperature_variance = param_temperature_variance.get();

        while (true)
        {
            watchdog_.reset();
            ::usleep(500000);
            watchdog_.reset();

            if (!ms5611Reset(&sens))
            {
                lowsyslog("Air sensor reset failed, will retry...\n");
                continue;
            }

            if (!ms5611GetProm(&sens))
            {
                lowsyslog("Air sensor PROM read failed, will retry...\n");
                continue;
            }

            node::setComponentStatus(node::ComponentID::AirSensor, uavcan::protocol::NodeStatus::STATUS_OK);
            tryRun();
            node::setComponentStatus(node::ComponentID::AirSensor, uavcan::protocol::NodeStatus::STATUS_CRITICAL);
            lowsyslog("Air sensor is about to restart...\n");
        }
        return msg_t();
    }
} air_sensor_thread;

}

void init()
{
    if (param_enabled)
    {
        (void)air_sensor_thread.start(HIGHPRIO - 10);
    }
    else
    {
        node::setComponentStatus(node::ComponentID::AirSensor, uavcan::protocol::NodeStatus::STATUS_OK);
        lowsyslog("Air sensor disabled\n");
    }
}

}
