// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <gtest/gtest.h>

#include <everest/ocpp_module_common/device_model/telemetry_device_model_storage.hpp>

namespace {

using ocpp_module_common::device_model::TelemetryDeviceModelStorage;
using ocpp_module_common::device_model::TelemetryMapping;
using ocpp_module_common::device_model::VARIABLE_SOURCE_TELEMETRY;
using ocpp_module_common::otlp::Export;
using ocpp_module_common::otlp::Sample;

/// \brief A mapping onto \p metric, described the way the mapping file would describe it.
TelemetryMapping mapping_of(const std::string& component, const std::string& variable, const std::string& metric,
                            ocpp::v2::DataEnum type = ocpp::v2::DataEnum::decimal,
                            std::map<std::string, std::string> attributes = {},
                            std::optional<std::int32_t> evse = std::nullopt) {
    TelemetryMapping mapping;
    mapping.component.name = component;
    if (evse.has_value()) {
        ocpp::v2::EVSE id;
        id.id = *evse;
        mapping.component.evse = id;
    }
    mapping.variable.name = variable;
    mapping.selector.metric = metric;
    mapping.selector.attributes = std::move(attributes);
    mapping.characteristics.dataType = type;
    mapping.characteristics.supportsMonitoring = true;
    mapping.characteristics.unit = "Celsius";
    mapping.characteristics.minLimit = -40;
    mapping.characteristics.maxLimit = 120;
    return mapping;
}

/// \brief One export carrying one double data point.
Export exported(const std::string& metric, double value, std::map<std::string, std::string> attributes = {}) {
    Sample sample;
    sample.metric = metric;
    sample.as_double = value;
    sample.attributes = std::move(attributes);
    Export out;
    out.samples.push_back(std::move(sample));
    return out;
}

std::optional<std::string> value_of(TelemetryDeviceModelStorage& storage, const TelemetryMapping& mapping) {
    const auto attribute =
        storage.get_variable_attribute(mapping.component, mapping.variable, ocpp::v2::AttributeEnum::Actual);
    return attribute.has_value() ? attribute->value : std::nullopt;
}

// The characteristics a CSMS is told come from the mapping file, because OTLP carries none of them.
TEST(TelemetryDeviceModelStorage, TakesCharacteristicsFromTheMapping) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature");
    TelemetryDeviceModelStorage storage({temperature});

    const auto model = storage.get_device_model();
    ASSERT_EQ(model.size(), 1);
    const auto& variables = model.at(temperature.component);
    ASSERT_EQ(variables.size(), 1);

    const auto& meta = variables.at(temperature.variable);
    EXPECT_EQ(meta.characteristics.dataType, ocpp::v2::DataEnum::decimal);
    EXPECT_EQ(meta.characteristics.unit.value(), "Celsius");
    EXPECT_TRUE(meta.characteristics.supportsMonitoring);
    ASSERT_TRUE(meta.source.has_value());
    EXPECT_EQ(meta.source.value(), VARIABLE_SOURCE_TELEMETRY);
}

// A mapped variable exists before anything has been measured, which is what lets a GetBaseReport at
// boot be complete. It reads as absent rather than as a value the station never took.
TEST(TelemetryDeviceModelStorage, ReportsNoValueBeforeTheFirstExport) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature");
    TelemetryDeviceModelStorage storage({temperature});

    EXPECT_EQ(storage.get_device_model().size(), 1);
    EXPECT_FALSE(value_of(storage, temperature).has_value());
}

TEST(TelemetryDeviceModelStorage, ServesTheLastValueOfAMappedMetric) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature");
    TelemetryDeviceModelStorage storage({temperature});

    EXPECT_EQ(storage.on_export(exported("powermeter.temperature", 40.5)), 1);
    EXPECT_EQ(value_of(storage, temperature).value(), "40.5");

    // the newest value replaces the last, which is the whole contract of a last-value store
    EXPECT_EQ(storage.on_export(exported("powermeter.temperature", 41.25)), 1);
    EXPECT_EQ(value_of(storage, temperature).value(), "41.25");
}

// Anything may push a metric; only what the file names becomes a variable.
TEST(TelemetryDeviceModelStorage, IgnoresAMetricNoMappingNames) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature");
    TelemetryDeviceModelStorage storage({temperature});

    EXPECT_EQ(storage.on_export(exported("powermeter.humidity", 61.0)), 0);
    EXPECT_FALSE(value_of(storage, temperature).has_value());
}

// One metric, one data point per EVSE: the attributes are what tell them apart.
TEST(TelemetryDeviceModelStorage, SelectsADataPointByItsAttributes) {
    const auto evse1 = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature",
                                  ocpp::v2::DataEnum::decimal, {{"evse", "1"}}, 1);
    const auto evse2 = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature",
                                  ocpp::v2::DataEnum::decimal, {{"evse", "2"}}, 2);
    TelemetryDeviceModelStorage storage({evse1, evse2});

    Export both = exported("powermeter.temperature", 40.5, {{"evse", "1"}});
    Sample second;
    second.metric = "powermeter.temperature";
    second.as_double = 31.0;
    second.attributes = {{"evse", "2"}};
    both.samples.push_back(second);

    EXPECT_EQ(storage.on_export(both), 2);
    EXPECT_EQ(value_of(storage, evse1).value(), "40.5");
    EXPECT_EQ(value_of(storage, evse2).value(), "31");

    // an attribute the mapping does not name is ignored, so a producer may add dimensions freely
    Export extra = exported("powermeter.temperature", 44.0, {{"evse", "1"}, {"phase", "L2"}});
    EXPECT_EQ(storage.on_export(extra), 1);
    EXPECT_EQ(value_of(storage, evse1).value(), "44");

    // and a point missing an attribute the mapping requires matches nothing
    EXPECT_EQ(storage.on_export(exported("powermeter.temperature", 99.0)), 0);
    EXPECT_EQ(value_of(storage, evse1).value(), "44");
}

// The data type the mapping declares decides how the value reads, so a CSMS is never told a
// decimal for a variable it was told is an integer.
TEST(TelemetryDeviceModelStorage, RendersAValueAsTheDeclaredDataType) {
    const auto memory = mapping_of("Controller", "MemoryUsed", "system.memory.usage", ocpp::v2::DataEnum::integer);
    TelemetryDeviceModelStorage storage({memory});

    Sample sample;
    sample.metric = "system.memory.usage";
    sample.is_integer = true;
    sample.as_int = 995311616;
    Export out;
    out.samples.push_back(sample);

    EXPECT_EQ(storage.on_export(out), 1);
    EXPECT_EQ(value_of(storage, memory).value(), "995311616");

    // a producer that sends the same thing as a double still reads as an integer
    EXPECT_EQ(storage.on_export(exported("system.memory.usage", 42.7)), 1);
    EXPECT_EQ(value_of(storage, memory).value(), "43");
}

TEST(TelemetryDeviceModelStorage, RejectsAWriteFromTheCsms) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature");
    TelemetryDeviceModelStorage storage({temperature});

    // Telemetry flows one way: a CSMS that could write a measurement back could make the station
    // report a value it never took.
    EXPECT_EQ(storage.set_variable_attribute_value(temperature.component, temperature.variable,
                                                   ocpp::v2::AttributeEnum::Actual, "1.0", "CSMS"),
              ocpp::v2::SetVariableStatusEnum::Rejected);
}

ocpp::v2::SetMonitoringData monitor_request(const TelemetryMapping& mapping, ocpp::v2::MonitorEnum type,
                                            float value) {
    ocpp::v2::SetMonitoringData request;
    request.component = mapping.component;
    request.variable = mapping.variable;
    request.type = type;
    request.severity = 5;
    request.value = value;
    return request;
}

// The component config these variables are seeded into the device model database as. Getting this
// wrong means the VARIABLE row is missing and a monitor has no row to hang off.
TEST(TelemetryDeviceModelStorage, DescribesItsVariablesAsAComponentConfig) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature",
                                        ocpp::v2::DataEnum::decimal, {}, 1);
    TelemetryDeviceModelStorage storage({temperature});

    const auto config = storage.component_config();
    ASSERT_EQ(config.size(), 1);
    const auto& [component, variables] = *config.begin();
    EXPECT_EQ(component.name, "PowerMeterDC");
    EXPECT_EQ(component.evse_id.value(), 1);
    ASSERT_EQ(variables.size(), 1);

    const auto& variable = variables.front();
    EXPECT_EQ(variable.name, "MeterTemperature");
    // The source on the row is what routes reads back to this storage once it is registered.
    ASSERT_TRUE(variable.source.has_value());
    EXPECT_EQ(variable.source.value(), VARIABLE_SOURCE_TELEMETRY);
    EXPECT_EQ(variable.characteristics.dataType, ocpp::v2::DataEnum::decimal);
    ASSERT_EQ(variable.attributes.size(), 1);
    EXPECT_EQ(variable.attributes.front().variable_attribute.mutability, ocpp::v2::MutabilityEnum::ReadOnly);
    // Never written, so never persisted, and no default value the station did not measure.
    EXPECT_FALSE(variable.attributes.front().variable_attribute.persistent.value());
    EXPECT_FALSE(variable.attributes.front().variable_attribute.value.has_value());
}

// Records what it was asked, so the tests can assert the call was forwarded rather than answered.
class FakeMonitorStore : public ocpp::v2::DeviceModelStorageInterface {
public:
    std::vector<ocpp::v2::SetMonitoringData> set_requests;
    std::vector<int> cleared;

    ocpp::v2::DeviceModelMap get_device_model() override {
        return {};
    }
    std::optional<ocpp::v2::VariableAttribute> get_variable_attribute(const ocpp::v2::Component&,
                                                                      const ocpp::v2::Variable&,
                                                                      const ocpp::v2::AttributeEnum&) override {
        return std::nullopt;
    }
    std::vector<ocpp::v2::VariableAttribute>
    get_variable_attributes(const ocpp::v2::Component&, const ocpp::v2::Variable&,
                            const std::optional<ocpp::v2::AttributeEnum>&) override {
        return {};
    }
    ocpp::v2::SetVariableStatusEnum set_variable_attribute_value(const ocpp::v2::Component&, const ocpp::v2::Variable&,
                                                                 const ocpp::v2::AttributeEnum&, const std::string&,
                                                                 const std::string&) override {
        return ocpp::v2::SetVariableStatusEnum::Rejected;
    }
    std::optional<ocpp::v2::VariableMonitoringMeta>
    set_monitoring_data(const ocpp::v2::SetMonitoringData& data, const ocpp::v2::VariableMonitorType type) override {
        this->set_requests.push_back(data);
        ocpp::v2::VariableMonitoringMeta meta;
        meta.type = type;
        meta.monitor.id = 7;
        meta.monitor.type = data.type;
        return meta;
    }
    bool update_monitoring_reference(const std::int32_t, const std::string&) override {
        return true;
    }
    std::vector<ocpp::v2::VariableMonitoringMeta>
    get_monitoring_data(const std::vector<ocpp::v2::MonitoringCriterionEnum>&, const ocpp::v2::Component&,
                        const ocpp::v2::Variable&) override {
        return {ocpp::v2::VariableMonitoringMeta{}};
    }
    ocpp::v2::ClearMonitoringStatusEnum clear_variable_monitor(int monitor_id, bool) override {
        this->cleared.push_back(monitor_id);
        return ocpp::v2::ClearMonitoringStatusEnum::Accepted;
    }
    std::int32_t clear_custom_variable_monitors() override {
        return 4;
    }
    void check_integrity() override {
    }
};

// Monitors belong in the database the variables were seeded into, so that they persist and so that
// their ids come from the same sequence as every other monitor.
TEST(TelemetryDeviceModelStorage, ForwardsMonitorsToTheSeededStore) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature");
    TelemetryDeviceModelStorage storage({temperature});
    auto store = std::make_shared<FakeMonitorStore>();
    storage.set_monitor_store(store);

    const auto monitor = storage.set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Periodic, 5),
                                                     ocpp::v2::VariableMonitorType::CustomMonitor);
    ASSERT_TRUE(monitor.has_value());
    EXPECT_EQ(monitor->monitor.id, 7);
    ASSERT_EQ(store->set_requests.size(), 1);

    EXPECT_EQ(storage.get_monitoring_data({}, temperature.component, temperature.variable).size(), 1);
    EXPECT_EQ(storage.clear_variable_monitor(7, true), ocpp::v2::ClearMonitoringStatusEnum::Accepted);
    EXPECT_EQ(store->cleared, std::vector<int>{7});
    EXPECT_TRUE(storage.update_monitoring_reference(7, "1"));

    // The shared store is asked for its custom monitors once, by the source that owns it. Answering
    // here as well would clear them once and count them twice.
    EXPECT_EQ(storage.clear_custom_variable_monitors(), 0);
}

// A delta monitor is valid before the first value arrives; it simply cannot fire yet. Refusing it
// would deny the CSMS a monitor it is entitled to.
TEST(TelemetryDeviceModelStorage, AcceptsADeltaMonitorBeforeAnyValueHasArrived) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature");
    TelemetryDeviceModelStorage storage({temperature});
    auto store = std::make_shared<FakeMonitorStore>();
    storage.set_monitor_store(store);

    EXPECT_TRUE(storage
                    .set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Delta, 1),
                                         ocpp::v2::VariableMonitorType::CustomMonitor)
                    .has_value());
}

TEST(TelemetryDeviceModelStorage, AnswersNoMonitorCallWithoutAStore) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "powermeter.temperature");
    TelemetryDeviceModelStorage storage({temperature});

    EXPECT_FALSE(storage
                     .set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Periodic, 5),
                                          ocpp::v2::VariableMonitorType::CustomMonitor)
                     .has_value());
    EXPECT_EQ(storage.clear_variable_monitor(7, true), ocpp::v2::ClearMonitoringStatusEnum::NotFound);
}

} // namespace
