// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <gtest/gtest.h>

#include <everest/ocpp_module_common/device_model/telemetry_device_model_storage.hpp>

namespace {

using ocpp_module_common::device_model::TelemetryDeviceModelStorage;
using ocpp_module_common::device_model::TelemetryMapping;
using ocpp_module_common::device_model::VARIABLE_SOURCE_TELEMETRY;

const Everest::telemetry::SetKey LIVEDATA{"powermeter_1", "livedata"};

types::telemetry::EntryDefinition entry(const std::string& name, types::telemetry::EntryType type) {
    types::telemetry::EntryDefinition definition;
    definition.name = name;
    definition.description = name;
    definition.type = type;
    return definition;
}

/// One set declaring a bounded temperature, an integer count and a state with an allowed-value list.
types::telemetry::SetDefinition livedata() {
    types::telemetry::SetDefinition definition;
    definition.module_type = "PowerMeterExample";
    definition.set = "livedata";
    definition.description = "live";

    auto temperature = entry("temperature_C", types::telemetry::EntryType::number);
    temperature.unit = "Celsius";
    temperature.minimum = -40;
    temperature.maximum = 120;

    auto count = entry("error_count", types::telemetry::EntryType::integer);
    count.minimum = 0;

    auto state = entry("fw_state", types::telemetry::EntryType::string);
    state.values_list = std::vector<std::string>{"Idle", "Measuring"};

    definition.entries = {temperature, count, state};
    return definition;
}

std::map<Everest::telemetry::SetKey, types::telemetry::SetDefinition> definitions() {
    return {{LIVEDATA, livedata()}};
}

TelemetryMapping mapping_of(const std::string& component, const std::string& variable, const std::string& entry_name,
                            std::optional<std::int32_t> evse = std::nullopt) {
    TelemetryMapping mapping;
    mapping.component.name = component;
    if (evse.has_value()) {
        ocpp::v2::EVSE id;
        id.id = *evse;
        mapping.component.evse = id;
    }
    mapping.variable.name = variable;
    mapping.flow = LIVEDATA;
    mapping.entry = entry_name;
    return mapping;
}

types::telemetry::Update update_of(json::object_t values) {
    types::telemetry::Update update;
    update.module_id = LIVEDATA.module_id;
    update.module_type = "PowerMeterExample";
    update.set = LIVEDATA.set;
    update.timestamp = "2026-09-09T10:00:00Z";
    update.values = std::move(values);
    return update;
}

ocpp::v2::ComponentVariable target_of(const TelemetryMapping& mapping) {
    return ocpp::v2::ComponentVariable{mapping.component, mapping.variable, std::nullopt};
}

// The declaration the producer answers with is what describes the variable, not the mapping file.
TEST(TelemetryDeviceModelStorage, DerivesCharacteristicsFromTheSetDeclaration) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C", 1);
    const auto state = mapping_of("PowerMeterDC", "FirmwareState", "fw_state", 1);
    TelemetryDeviceModelStorage storage({temperature, state}, definitions());

    const auto model = storage.get_device_model();
    ASSERT_EQ(model.size(), 1);
    const auto& variables = model.at(temperature.component);
    ASSERT_EQ(variables.size(), 2);

    const auto& meta = variables.at(temperature.variable);
    EXPECT_EQ(meta.characteristics.dataType, ocpp::v2::DataEnum::decimal);
    EXPECT_TRUE(meta.characteristics.supportsMonitoring);
    ASSERT_TRUE(meta.characteristics.unit.has_value());
    EXPECT_EQ(meta.characteristics.unit->get(), "Celsius");
    EXPECT_FLOAT_EQ(meta.characteristics.minLimit.value(), -40.0F);
    EXPECT_FLOAT_EQ(meta.characteristics.maxLimit.value(), 120.0F);
    // Routing depends on this: the composed storage reads the source out of the declaration.
    ASSERT_TRUE(meta.source.has_value());
    EXPECT_EQ(meta.source.value(), VARIABLE_SOURCE_TELEMETRY);

    // A string entry with an allowed-value list is an OptionList, so a CSMS can render the choice.
    const auto& state_meta = variables.at(state.variable);
    EXPECT_EQ(state_meta.characteristics.dataType, ocpp::v2::DataEnum::OptionList);
    ASSERT_TRUE(state_meta.characteristics.valuesList.has_value());
    EXPECT_EQ(state_meta.characteristics.valuesList->get(), "Idle,Measuring");
}

TEST(TelemetryDeviceModelStorage, DropsAMappingTheSetDoesNotDeclare) {
    const auto good = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    const auto bad = mapping_of("PowerMeterDC", "Nonsense", "no_such_entry");
    TelemetryDeviceModelStorage storage({good, bad}, definitions());

    EXPECT_EQ(storage.mappings().size(), 1);
    ASSERT_EQ(storage.unavailable().size(), 1);
    EXPECT_NE(storage.unavailable().front().find("no_such_entry"), std::string::npos);
}

// A value that has never been published is absent, not zero: a CSMS cannot tell a made-up number
// from a measured one.
TEST(TelemetryDeviceModelStorage, ReportsNoValueBeforeTheFirstUpdate) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    const auto attribute =
        storage.get_variable_attribute(temperature.component, temperature.variable, ocpp::v2::AttributeEnum::Actual);
    ASSERT_TRUE(attribute.has_value());
    EXPECT_FALSE(attribute->value.has_value());
    EXPECT_EQ(attribute->mutability, ocpp::v2::MutabilityEnum::ReadOnly);
    EXPECT_FALSE(attribute->persistent.value());
}

TEST(TelemetryDeviceModelStorage, ServesTheLastValueOfAMappedEntry) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    EXPECT_EQ(storage.on_update(update_of({{"temperature_C", 40.5}, {"error_count", 3}})), 1);
    const auto attribute =
        storage.get_variable_attribute(temperature.component, temperature.variable, ocpp::v2::AttributeEnum::Actual);
    ASSERT_TRUE(attribute->value.has_value());
    EXPECT_EQ(attribute->value->get(), "40.5");

    // Only Actual exists. A measurement has no target or setpoint.
    EXPECT_FALSE(
        storage.get_variable_attribute(temperature.component, temperature.variable, ocpp::v2::AttributeEnum::Target)
            .has_value());
}

TEST(TelemetryDeviceModelStorage, RejectsAWriteFromTheCsms) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    EXPECT_EQ(storage.set_variable_attribute_value(temperature.component, temperature.variable,
                                                   ocpp::v2::AttributeEnum::Actual, "99", "CSMS"),
              ocpp::v2::SetVariableStatusEnum::Rejected);
}

ocpp::v2::SetMonitoringData monitor_request(const TelemetryMapping& mapping, ocpp::v2::MonitorEnum type, float value) {
    ocpp::v2::SetMonitoringData request;
    request.component = mapping.component;
    request.variable = mapping.variable;
    request.type = type;
    request.severity = 5;
    request.value = value;
    return request;
}

TEST(TelemetryDeviceModelStorage, HoldsMonitorsOutsideTheSqliteIdRange) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    const auto monitor = storage.set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Periodic, 5),
                                                     ocpp::v2::VariableMonitorType::CustomMonitor);
    ASSERT_TRUE(monitor.has_value());
    // The OCPP-source storage numbers monitors with SQLite row ids from 1. An overlap would make
    // ClearVariableMonitoring ambiguous, because the id is all the CSMS sends.
    EXPECT_GT(monitor->monitor.id, 0x10000000);

    const auto found = storage.get_monitoring_data({}, temperature.component, temperature.variable);
    ASSERT_EQ(found.size(), 1);
    EXPECT_EQ(found.front().monitor.id, monitor->monitor.id);

    // Criteria filter on the monitor type.
    EXPECT_EQ(storage.get_monitoring_data({ocpp::v2::MonitoringCriterionEnum::PeriodicMonitoring},
                                          temperature.component, temperature.variable)
                  .size(),
              1);
    EXPECT_TRUE(storage.get_monitoring_data({ocpp::v2::MonitoringCriterionEnum::DeltaMonitoring},
                                            temperature.component, temperature.variable)
                    .empty());
}

// A delta is measured from a reference, so there has to be one.
TEST(TelemetryDeviceModelStorage, RefusesADeltaMonitorUntilAValueHasArrived) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    EXPECT_FALSE(storage
                     .set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Delta, 1),
                                          ocpp::v2::VariableMonitorType::CustomMonitor)
                     .has_value());

    storage.on_update(update_of({{"temperature_C", 40.5}}));
    const auto monitor = storage.set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Delta, 1),
                                                     ocpp::v2::VariableMonitorType::CustomMonitor);
    ASSERT_TRUE(monitor.has_value());
    ASSERT_TRUE(monitor->reference_value.has_value());
    EXPECT_EQ(monitor->reference_value.value(), "40.5");

    EXPECT_TRUE(storage.update_monitoring_reference(monitor->monitor.id, "41.5"));
    EXPECT_EQ(storage.get_monitoring_data({}, temperature.component, temperature.variable)
                  .front()
                  .reference_value.value(),
              "41.5");
    EXPECT_FALSE(storage.update_monitoring_reference(monitor->monitor.id + 1, "0"));
}

TEST(TelemetryDeviceModelStorage, ClearsOnlyItsOwnMonitors) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    const auto monitor = storage.set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Periodic, 5),
                                                     ocpp::v2::VariableMonitorType::CustomMonitor);
    ASSERT_TRUE(monitor.has_value());

    // An id from another source is NotFound, not Rejected, so the composed storage keeps asking.
    EXPECT_EQ(storage.clear_variable_monitor(1, true), ocpp::v2::ClearMonitoringStatusEnum::NotFound);
    EXPECT_EQ(storage.clear_variable_monitor(monitor->monitor.id, true),
              ocpp::v2::ClearMonitoringStatusEnum::Accepted);
    EXPECT_TRUE(storage.get_monitoring_data({}, temperature.component, temperature.variable).empty());
}

TEST(TelemetryDeviceModelStorage, ClearCustomLeavesPreconfiguredMonitorsAlone) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    storage.set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Periodic, 5),
                                ocpp::v2::VariableMonitorType::CustomMonitor);
    storage.set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::UpperThreshold, 50),
                                ocpp::v2::VariableMonitorType::PreconfiguredMonitor);

    EXPECT_EQ(storage.clear_custom_variable_monitors(), 1);
    const auto left = storage.get_monitoring_data({}, temperature.component, temperature.variable);
    ASSERT_EQ(left.size(), 1);
    EXPECT_EQ(left.front().type, ocpp::v2::VariableMonitorType::PreconfiguredMonitor);
}

TEST(TelemetryDeviceModelStorage, RefusesAMonitorOnAnUnmappedTarget) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    auto elsewhere = mapping_of("SomethingElse", "Whatever", "temperature_C");
    EXPECT_FALSE(storage
                     .set_monitoring_data(monitor_request(elsewhere, ocpp::v2::MonitorEnum::Periodic, 5),
                                          ocpp::v2::VariableMonitorType::CustomMonitor)
                     .has_value());
}

} // namespace
