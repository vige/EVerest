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

// The component config these variables are seeded into the device model database as. Getting this
// wrong means the VARIABLE row is missing and a monitor has no row to hang off.
TEST(TelemetryDeviceModelStorage, DescribesItsVariablesAsAComponentConfig) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C", 1);
    TelemetryDeviceModelStorage storage({temperature}, definitions());

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
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());
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
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());
    auto store = std::make_shared<FakeMonitorStore>();
    storage.set_monitor_store(store);

    EXPECT_TRUE(storage
                    .set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Delta, 1),
                                         ocpp::v2::VariableMonitorType::CustomMonitor)
                    .has_value());
}

TEST(TelemetryDeviceModelStorage, AnswersNoMonitorCallWithoutAStore) {
    const auto temperature = mapping_of("PowerMeterDC", "MeterTemperature", "temperature_C");
    TelemetryDeviceModelStorage storage({temperature}, definitions());

    EXPECT_FALSE(storage
                     .set_monitoring_data(monitor_request(temperature, ocpp::v2::MonitorEnum::Periodic, 5),
                                          ocpp::v2::VariableMonitorType::CustomMonitor)
                     .has_value());
    EXPECT_EQ(storage.clear_variable_monitor(7, true), ocpp::v2::ClearMonitoringStatusEnum::NotFound);
}

} // namespace
