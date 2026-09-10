// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <generated/types/telemetry.hpp>
#include <ocpp/v2/device_model_storage_interface.hpp>
#include <ocpp/v2/device_model_storage_sqlite.hpp>
#include <ocpp/v2/init_device_model_db.hpp>

#include <everest/ocpp_module_common/device_model/telemetry_mapping.hpp>

namespace ocpp_module_common::device_model {

/// \brief The source id this storage registers under in the composed device model storage.
inline constexpr auto VARIABLE_SOURCE_TELEMETRY = "TELEMETRY";

/// \brief Device model storage for the telemetry entries a mapping file exposes to the CSMS.
///
/// Unlike the two storages beside it, this one declares its own structure: get_device_model() returns
/// a component/variable for every mapping whose entry the publishing set actually declares, with the
/// data type, unit and bounds taken from that declaration. There is no component config JSON to keep
/// in step with the producers, and no way for the mapping to describe a value differently from the
/// module that publishes it. The composed storage reads the "TELEMETRY" source out of this map at
/// registration time, which is what routes reads of those variables back here.
///
/// Nothing is persisted. A telemetry set can change several times a second, and a device model
/// variable is only ever read on request, so the values live in a map that the sink's update handler
/// writes and a read copies out of. The value of an entry that has not arrived yet is absent rather
/// than zero: reporting a number the station never measured would be a lie the CSMS cannot detect.
///
/// Monitors are not held here. component_config() describes these variables the way a component
/// config JSON would, and the caller seeds those rows into the device model database before the
/// charge point is built, so a telemetry variable has a real VARIABLE row like any other. Monitor
/// calls are then delegated to the storage that owns that database: monitors persist across a
/// reboot, their ids come from the one sequence that also numbers every other monitor -- so a
/// ClearVariableMonitoring id can never be ambiguous -- and dropping a variable that is no longer
/// mapped takes its monitors with it through the foreign key.
///
/// A read must never issue an EVerest framework command: libocpp calls it while holding the device
/// model lock, and a command reply travels back through the MQTT machinery whose handler thread may
/// be the caller. A read here takes this class's own mutex and does a map lookup, and nothing else.
class TelemetryDeviceModelStorage : public ocpp::v2::DeviceModelStorageInterface {
public:
    /// \brief Builds the device model of \p mappings, keeping those whose set declares the entry.
    /// \param mappings the validated mapping list
    /// \param definitions the set definitions the sink resolved, keyed by flow
    TelemetryDeviceModelStorage(const std::vector<TelemetryMapping>& mappings,
                                const std::map<Everest::telemetry::SetKey, types::telemetry::SetDefinition>& definitions);

    /// \brief These variables as a component config, for seeding the device model database.
    ///
    /// Merge into what get_all_component_configs() read from the config directory and initialise
    /// once over the union, so the integrity check sees a whole device model rather than a slice.
    std::map<ocpp::v2::ComponentKey, std::vector<ocpp::v2::DeviceModelVariable>> component_config() const;

    /// \brief Names the storage that owns the database the rows were seeded into.
    ///
    /// Every monitor call is forwarded there. Without one this storage answers no monitor call,
    /// which is what an unseeded station gets.
    void set_monitor_store(std::shared_ptr<ocpp::v2::DeviceModelStorageInterface> store);
    virtual ~TelemetryDeviceModelStorage() override = default;

    /// \brief Records the values of \p update that some mapping reads from. Called from the sink.
    /// \returns the number of device model variables the update changed
    std::size_t on_update(const types::telemetry::Update& update);

    /// \brief The mappings this storage kept, in target order. For logs and introspection.
    const std::map<ocpp::v2::ComponentVariable, TelemetryMapping>& mappings() const;

    /// \brief The mappings dropped because the set does not declare the entry, with the reason.
    const std::vector<std::string>& unavailable() const;

    virtual ocpp::v2::DeviceModelMap get_device_model() override;
    virtual std::optional<ocpp::v2::VariableAttribute>
    get_variable_attribute(const ocpp::v2::Component& component_id, const ocpp::v2::Variable& variable_id,
                           const ocpp::v2::AttributeEnum& attribute_enum) override;
    virtual std::vector<ocpp::v2::VariableAttribute>
    get_variable_attributes(const ocpp::v2::Component& component_id, const ocpp::v2::Variable& variable_id,
                            const std::optional<ocpp::v2::AttributeEnum>& attribute_enum) override;
    virtual ocpp::v2::SetVariableStatusEnum set_variable_attribute_value(const ocpp::v2::Component& component_id,
                                                                         const ocpp::v2::Variable& variable_id,
                                                                         const ocpp::v2::AttributeEnum& attribute_enum,
                                                                         const std::string& value,
                                                                         const std::string& source) override;
    virtual std::optional<ocpp::v2::VariableMonitoringMeta>
    set_monitoring_data(const ocpp::v2::SetMonitoringData& data, const ocpp::v2::VariableMonitorType type) override;
    virtual bool update_monitoring_reference(const int32_t monitor_id, const std::string& reference_value) override;
    virtual std::vector<ocpp::v2::VariableMonitoringMeta>
    get_monitoring_data(const std::vector<ocpp::v2::MonitoringCriterionEnum>& criteria,
                        const ocpp::v2::Component& component_id, const ocpp::v2::Variable& variable_id) override;
    virtual ocpp::v2::ClearMonitoringStatusEnum clear_variable_monitor(int monitor_id, bool allow_protected) override;
    virtual int32_t clear_custom_variable_monitors() override;
    virtual void check_integrity() override;

private:
    /// \returns the string form of the value of \p mapping, or nothing when it has not arrived yet
    std::optional<std::string> read(const TelemetryMapping& mapping) const;


    std::map<ocpp::v2::ComponentVariable, TelemetryMapping> table;
    /// target -> the entry type its set declared, which is how a value is rendered
    std::map<ocpp::v2::ComponentVariable, types::telemetry::EntryType> entry_types;
    ocpp::v2::DeviceModelMap model;
    std::vector<std::string> dropped;

    mutable std::mutex mutex;
    /// flow -> entry -> last value seen, as the string the device model reports
    std::map<Everest::telemetry::SetKey, std::map<std::string, std::string>> values;

    /// The storage owning the database these variables were seeded into. Null until set.
    std::shared_ptr<ocpp::v2::DeviceModelStorageInterface> monitor_store;
};

/// \brief Initialises the device model database from \p config_path plus the telemetry components of
/// \p telemetry, and returns the storage connected to it.
///
/// One initialisation over the union of the two, because the integrity check wants a whole device
/// model rather than a slice. Seeding the telemetry variables as ordinary rows is what lets their
/// monitors live in the ordinary monitor table, numbered from the ordinary id sequence. The returned
/// storage is bound to \p telemetry as its monitor store.
///
/// With a null \p telemetry this is exactly the stock three-argument construction.
std::shared_ptr<ocpp::v2::DeviceModelStorageSqlite>
make_ocpp_device_model_storage(const std::filesystem::path& database_path,
                               const std::filesystem::path& migration_path,
                               const std::filesystem::path& config_path,
                               const std::shared_ptr<TelemetryDeviceModelStorage>& telemetry);

/// \returns the string form of the JSON scalar \p value as the device model reports it
///
/// Exposed for the unit tests: the rendering of a number is the difference between a CSMS seeing
/// 40.5 and seeing 40.500000, and between an integer entry arriving as 7 and as 7.0.
std::string render_value(const nlohmann::json& value, types::telemetry::EntryType type);

} // namespace ocpp_module_common::device_model
