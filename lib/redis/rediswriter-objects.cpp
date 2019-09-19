/******************************************************************************
 * Icinga 2                                                                   *
 * Copyright (C) 2012-2018 Icinga Development Team (https://www.icinga.com/)  *
 *                                                                            *
 * This program is free software; you can redistribute it and/or              *
 * modify it under the terms of the GNU General Public License                *
 * as published by the Free Software Foundation; either version 2             *
 * of the License, or (at your option) any later version.                     *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * GNU General Public License for more details.                               *
 *                                                                            *
 * You should have received a copy of the GNU General Public License          *
 * along with this program; if not, write to the Free Software Foundation     *
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.             *
 ******************************************************************************/

#include "redis/rediswriter.hpp"
#include "redis/redisconnection.hpp"
#include "icinga/command.hpp"
#include "icinga/compatutility.hpp"
#include "base/configtype.hpp"
#include "base/configobject.hpp"
#include "icinga/customvarobject.hpp"
#include "icinga/host.hpp"
#include "icinga/service.hpp"
#include "icinga/hostgroup.hpp"
#include "icinga/servicegroup.hpp"
#include "icinga/usergroup.hpp"
#include "icinga/checkcommand.hpp"
#include "icinga/eventcommand.hpp"
#include "icinga/notificationcommand.hpp"
#include "icinga/timeperiod.hpp"
#include "icinga/pluginutility.hpp"
#include "remote/zone.hpp"
#include "base/json.hpp"
#include "base/logger.hpp"
#include "base/serializer.hpp"
#include "base/tlsutility.hpp"
#include "base/initialize.hpp"
#include "base/convert.hpp"
#include "base/array.hpp"
#include "base/exception.hpp"
#include "base/utility.hpp"
#include <iterator>
#include <map>
#include <set>
#include <utility>

using namespace icinga;

static const char * const l_LuaResetDump = R"EOF(

local id = redis.call('XADD', KEYS[1], '*', 'type', '*', 'state', 'wip')

local xr = redis.call('XRANGE', KEYS[1], '-', '+')
for i = 1, #xr - 1 do
	redis.call('XDEL', KEYS[1], xr[i][1])
end

return id

)EOF";

INITIALIZE_ONCE(&RedisWriter::ConfigStaticInitialize);

void RedisWriter::ConfigStaticInitialize()
{
	/* triggered in ProcessCheckResult(), requires UpdateNextCheck() to be called before */
	Checkable::OnStateChange.connect([](const Checkable::Ptr& checkable, const CheckResult::Ptr&, StateType, const MessageOrigin::Ptr&) {
		RedisWriter::StateChangeHandler(checkable);
	});

	/* triggered when acknowledged host/service goes back to ok and when the acknowledgement gets deleted */
	Checkable::OnAcknowledgementCleared.connect([](const Checkable::Ptr& checkable, const MessageOrigin::Ptr&) {
		RedisWriter::StateChangeHandler(checkable);
	});

	/* triggered on create, update and delete objects */
	ConfigObject::OnActiveChanged.connect([](const ConfigObject::Ptr& object, const Value&) {
		RedisWriter::VersionChangedHandler(object);
	});
	ConfigObject::OnVersionChanged.connect([](const ConfigObject::Ptr& object, const Value&) {
		RedisWriter::VersionChangedHandler(object);
	});

	/* fixed downtime start */
	Downtime::OnDowntimeStarted.connect(&RedisWriter::DowntimeChangedHandler);
	/* flexible downtime start */
	Downtime::OnDowntimeTriggered.connect(&RedisWriter::DowntimeChangedHandler);
	/* fixed/flexible downtime end */
	Downtime::OnDowntimeRemoved.connect(&RedisWriter::DowntimeChangedHandler);
}

void RedisWriter::UpdateAllConfigObjects()
{
	double startTime = Utility::GetTime();

	// Use a Workqueue to pack objects in parallel
	WorkQueue upq(25000, Configuration::Concurrency);
	upq.SetName("RedisWriter:ConfigDump");

	typedef std::pair<ConfigType *, String> TypePair;
	std::vector<TypePair> types;

	for (const Type::Ptr& type : Type::GetAllTypes()) {
		ConfigType *ctype = dynamic_cast<ConfigType *>(type.get());
		if (!ctype)
			continue;

		String lcType(type->GetName().ToLower());

		if (lcType == "downtime") {
			types.emplace_back(ctype, "hostdowntime");
			types.emplace_back(ctype, "servicedowntime");
		} else if (lcType == "comment") {
			types.emplace_back(ctype, "hostcomment");
			types.emplace_back(ctype, "servicecomment");
		} else {
			types.emplace_back(ctype, lcType);
		}
	}

	m_Rcon->FireAndForgetQuery({"EVAL", l_LuaResetDump, "1", "icinga:dump"});

	const std::vector<String> globalKeys = {
			m_PrefixConfigObject + "customvar",
			m_PrefixConfigObject + "action_url",
			m_PrefixConfigObject + "notes_url",
			m_PrefixConfigObject + "icon_image",
	};
	DeleteKeys(globalKeys);

	upq.ParallelFor(types, [this](const TypePair& type) {
		String lcType = type.second;

		std::vector<String> keys = GetTypeObjectKeys(lcType);
		DeleteKeys(keys);

		auto objectChunks (ChunkObjects(type.first->GetObjects(), 500));

		WorkQueue upqObjectType(25000, Configuration::Concurrency);
		upqObjectType.SetName("RedisWriter:ConfigDump:" + lcType);

		upqObjectType.ParallelFor(objectChunks, [this, &type, &lcType](decltype(objectChunks)::const_reference chunk) {
			std::map<String, std::vector<String>> hMSets, publishes;
			std::vector<String> states 							= {"HMSET", m_PrefixStateObject + lcType};
			std::vector<std::vector<String> > transaction 		= {{"MULTI"}};

			bool dumpState = (lcType == "host" || lcType == "service");

			size_t bulkCounter = 0;
			for (const ConfigObject::Ptr& object : chunk) {
				if (lcType != GetLowerCaseTypeNameDB(object))
					continue;

				CreateConfigUpdate(object, lcType, hMSets, publishes, false);

				// Write out inital state for checkables
				if (dumpState) {
					states.emplace_back(GetObjectIdentifier(object));
					states.emplace_back(JsonEncode(SerializeState(dynamic_pointer_cast<Checkable>(object))));
				}

				bulkCounter++;
				if (!(bulkCounter % 100)) {
					for (auto& kv : hMSets) {
						if (!kv.second.empty()) {
							kv.second.insert(kv.second.begin(), {"HMSET", kv.first});
							transaction.emplace_back(std::move(kv.second));
						}
					}

					if (states.size() > 2) {
						transaction.emplace_back(std::move(states));
						states = {"HMSET", m_PrefixStateObject + lcType};
					}

					for (auto& kv : publishes) {
						for (auto& message : kv.second) {
							std::vector<String> publish;

							publish.reserve(3);
							publish.emplace_back("PUBLISH");
							publish.emplace_back(kv.first);
							publish.emplace_back(std::move(message));

							transaction.emplace_back(std::move(publish));
						}
					}

					hMSets = decltype(hMSets)();
					publishes = decltype(publishes)();

					if (transaction.size() > 1) {
						transaction.push_back({"EXEC"});
						m_Rcon->FireAndForgetQueries(std::move(transaction));
						transaction = {{"MULTI"}};
					}
				}
			}

			for (auto& kv : hMSets) {
				if (!kv.second.empty()) {
					kv.second.insert(kv.second.begin(), {"HMSET", kv.first});
					transaction.emplace_back(std::move(kv.second));
				}
			}

			if (states.size() > 2)
				transaction.emplace_back(std::move(states));

			for (auto& kv : publishes) {
				for (auto& message : kv.second) {
					std::vector<String> publish;

					publish.reserve(3);
					publish.emplace_back("PUBLISH");
					publish.emplace_back(kv.first);
					publish.emplace_back(std::move(message));

					transaction.emplace_back(std::move(publish));
				}
			}

			if (transaction.size() > 1) {
				transaction.push_back({"EXEC"});
				m_Rcon->FireAndForgetQueries(std::move(transaction));
			}

			Log(LogNotice, "RedisWriter")
					<< "Dumped " << bulkCounter << " objects of type " << type.second;
		});

		upqObjectType.Join();

		if (upqObjectType.HasExceptions()) {
			for (boost::exception_ptr exc : upqObjectType.GetExceptions()) {
				if (exc) {
					boost::rethrow_exception(exc);
				}
			}
		}

		m_Rcon->FireAndForgetQuery({"XADD", "icinga:dump", "*", "type", lcType, "state", "done"});
	});

	upq.Join();

	if (upq.HasExceptions()) {
		for (boost::exception_ptr exc : upq.GetExceptions()) {
			try {
				if (exc) {
					boost::rethrow_exception(exc);
			}
			} catch(const std::exception& e) {
				Log(LogCritical, "RedisWriter")
						<< "Exception during ConfigDump: " << e.what();
			}
		}
	}

	m_Rcon->FireAndForgetQuery({"XADD", "icinga:dump", "*", "type", "*", "state", "done"});

	Log(LogInformation, "RedisWriter")
			<< "Initial config/status dump finished in " << Utility::GetTime() - startTime << " seconds.";
}

std::vector<std::vector<intrusive_ptr<ConfigObject>>> RedisWriter::ChunkObjects(std::vector<intrusive_ptr<ConfigObject>> objects, size_t chunkSize) {
	std::vector<std::vector<intrusive_ptr<ConfigObject>>> chunks;
	auto offset (objects.begin());
	auto end (objects.end());

	chunks.reserve((std::distance(offset, end) + chunkSize - 1) / chunkSize);

	while (std::distance(offset, end) >= chunkSize) {
		auto until (offset + chunkSize);
		chunks.emplace_back(offset, until);
		offset = until;
	}

	if (offset != end) {
		chunks.emplace_back(offset, end);
	}

	return std::move(chunks);
}

void RedisWriter::DeleteKeys(const std::vector<String>& keys) {
	std::vector<String> query = {"DEL"};
	for (auto& key : keys) {
		query.emplace_back(key);
	}

	m_Rcon->FireAndForgetQuery(std::move(query));
}

std::vector<String> RedisWriter::GetTypeObjectKeys(const String& type)
{
	std::vector<String> keys = {
			m_PrefixConfigObject + type,
			m_PrefixConfigCheckSum + type,
			m_PrefixConfigObject + type + ":customvar",
			m_PrefixConfigCheckSum + type + ":customvar",
	};

	if (type == "host" || type == "service" || type == "user") {
		keys.emplace_back(m_PrefixConfigObject + type + ":groupmember");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":groupmember");
		keys.emplace_back(m_PrefixStateObject + type);
	} else if (type == "timeperiod") {
		keys.emplace_back(m_PrefixConfigObject + type + ":override:include");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":override:include");
		keys.emplace_back(m_PrefixConfigObject + type + ":override:exclude");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":override:exclude");
		keys.emplace_back(m_PrefixConfigObject + type + ":range");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":range");
	} else if (type == "zone") {
		keys.emplace_back(m_PrefixConfigObject + type + ":parent");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":parent");
	} else if (type == "notification") {
		keys.emplace_back(m_PrefixConfigObject + type + ":user");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":user");
		keys.emplace_back(m_PrefixConfigObject + type + ":usergroup");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":usergroup");
	} else if (type == "checkcommand" || type == "notificationcommand" || type == "eventcommand") {
		keys.emplace_back(m_PrefixConfigObject + type + ":envvar");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":envvar");
		keys.emplace_back(m_PrefixConfigObject + type + ":argument");
		keys.emplace_back(m_PrefixConfigCheckSum + type + ":argument");
	}

	return std::move(keys);
}

template<typename ConfigType>
static ConfigObject::Ptr GetObjectByName(const String& name)
{
	return ConfigObject::GetObject<ConfigType>(name);
}

void RedisWriter::InsertObjectDependencies(const ConfigObject::Ptr& object, const String typeName, std::map<String, std::vector<String>>& hMSets,
		std::map<String, std::vector<String>>& publishes, bool runtimeUpdate)
{
	String objectKey = GetObjectIdentifier(object);
	CustomVarObject::Ptr customVarObject = dynamic_pointer_cast<CustomVarObject>(object);
	String envId = CalculateCheckSumString(GetEnvironment());
	auto* configUpdates (runtimeUpdate ? &publishes["icinga:config:update"] : nullptr);

	if (customVarObject) {
		auto vars(SerializeVars(customVarObject));
		if (vars) {
			auto& typeCvs (hMSets[m_PrefixConfigObject + typeName + ":customvar"]);
			auto& allCvs (hMSets[m_PrefixConfigObject + "customvar"]);
			auto& cvChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":customvar"]);

			cvChksms.emplace_back(objectKey);
			cvChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", CalculateCheckSumVars(customVarObject)}})));

			ObjectLock varsLock(vars);
			Array::Ptr varsArray(new Array);

			varsArray->Reserve(vars->GetLength());

			for (auto& kv : vars) {
				allCvs.emplace_back(kv.first);
				allCvs.emplace_back(JsonEncode(kv.second));

				if (configUpdates) {
					configUpdates->emplace_back("customvar:" + kv.first);
				}

				String id = CalculateCheckSumArray(new Array({envId, kv.first, objectKey}));
				typeCvs.emplace_back(id);
				typeCvs.emplace_back(JsonEncode(new Dictionary({{"object_id", objectKey}, {"env_id", envId}, {"customvar_id", kv.first}})));

				if (configUpdates) {
					configUpdates->emplace_back(typeName + ":customvar:" + id);
				}
			}
		}
	}

	Type::Ptr type = object->GetReflectionType();
	if (type == Host::TypeInstance || type == Service::TypeInstance) {
		Checkable::Ptr checkable = static_pointer_cast<Checkable>(object);

		String actionUrl = checkable->GetActionUrl();
		String notesUrl = checkable->GetNotesUrl();
		String iconImage = checkable->GetIconImage();
		if (!actionUrl.IsEmpty()) {
			auto& actionUrls (hMSets[m_PrefixConfigObject + "action_url"]);
			actionUrls.emplace_back(CalculateCheckSumArray(new Array({envId, actionUrl})));
			actionUrls.emplace_back(JsonEncode(new Dictionary({{"env_id", envId}, {"action_url", actionUrl}})));

			if (configUpdates) {
				configUpdates->emplace_back("action_url:" + actionUrls.at(actionUrls.size() - 2u));
			}
		}
		if (!notesUrl.IsEmpty()) {
			auto& notesUrls (hMSets[m_PrefixConfigObject + "notes_url"]);
			notesUrls.emplace_back(CalculateCheckSumArray(new Array({envId, notesUrl})));
			notesUrls.emplace_back(JsonEncode(new Dictionary({{"env_id", envId}, {"notes_url", notesUrl}})));

			if (configUpdates) {
				configUpdates->emplace_back("notes_url:" + notesUrls.at(notesUrls.size() - 2u));
			}
		}
		if (!iconImage.IsEmpty()) {
			auto& iconImages (hMSets[m_PrefixConfigObject + "icon_image"]);
			iconImages.emplace_back(CalculateCheckSumArray(new Array({envId, iconImage})));
			iconImages.emplace_back(JsonEncode(new Dictionary({{"env_id", envId}, {"icon_image", iconImage}})));

			if (configUpdates) {
				configUpdates->emplace_back("icon_image:" + iconImages.at(iconImages.size() - 2u));
			}
		}

		Host::Ptr host;
		Service::Ptr service;
		tie(host, service) = GetHostService(checkable);

		ConfigObject::Ptr (*getGroup)(const String& name);
		Array::Ptr groups;
		if (service) {
			groups = service->GetGroups();
			getGroup = &::GetObjectByName<ServiceGroup>;
		} else {
			groups = host->GetGroups();
			getGroup = &::GetObjectByName<HostGroup>;
		}

		if (groups) {
			ObjectLock groupsLock(groups);
			Array::Ptr groupIds(new Array);

			groupIds->Reserve(groups->GetLength());

			auto& members (hMSets[m_PrefixConfigObject + typeName + ":groupmember"]);
			auto& memberChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":groupmember"]);

			for (auto& group : groups) {
				String groupId = GetObjectIdentifier((*getGroup)(group));
				String id = CalculateCheckSumArray(new Array({envId, groupId, objectKey}));
				members.emplace_back(id);
				members.emplace_back(JsonEncode(new Dictionary({{"object_id", objectKey}, {"env_id", envId}, {"group_id", groupId}})));

				if (configUpdates) {
					configUpdates->emplace_back(typeName + ":groupmember:" + id);
				}

				groupIds->Add(groupId);
			}

			memberChksms.emplace_back(objectKey);
			memberChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", CalculateCheckSumArray(groupIds)}})));
		}

		return;
	}

	if (type == TimePeriod::TypeInstance) {
		TimePeriod::Ptr timeperiod = static_pointer_cast<TimePeriod>(object);

		Dictionary::Ptr ranges = timeperiod->GetRanges();
		if (ranges) {
			ObjectLock rangesLock(ranges);
			Array::Ptr rangeIds(new Array);
			auto& typeRanges (hMSets[m_PrefixConfigObject + typeName + ":range"]);
			auto& rangeChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":range"]);

			rangeIds->Reserve(ranges->GetLength());

			for (auto& kv : ranges) {
				String rangeId = CalculateCheckSumArray(new Array({envId, kv.first, kv.second}));
				rangeIds->Add(rangeId);

				String id = CalculateCheckSumArray(new Array({envId, rangeId, objectKey}));
				typeRanges.emplace_back(id);
				typeRanges.emplace_back(JsonEncode(new Dictionary({{"env_id", envId}, {"timeperiod_id", objectKey}, {"range_key", kv.first}, {"range_value", kv.second}})));

				if (configUpdates) {
					configUpdates->emplace_back(typeName + ":range:" + id);
				}
			}

			rangeChksms.emplace_back(objectKey);
			rangeChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", CalculateCheckSumArray(rangeIds)}})));
		}

		Array::Ptr includes;
		ConfigObject::Ptr (*getInclude)(const String& name);
		includes = timeperiod->GetIncludes();
		getInclude = &::GetObjectByName<TimePeriod>;

		Array::Ptr includeChecksums = new Array();

		ObjectLock includesLock(includes);
		ObjectLock includeChecksumsLock(includeChecksums);

		includeChecksums->Reserve(includes->GetLength());


		auto& includs (hMSets[m_PrefixConfigObject + typeName + ":override:include"]);
		auto& includeChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":override:include"]);
		for (auto include : includes) {
			String includeId = GetObjectIdentifier((*getInclude)(include.Get<String>()));
			includeChecksums->Add(includeId);

			String id = CalculateCheckSumArray(new Array({envId, includeId, objectKey}));
			includs.emplace_back(id);
			includs.emplace_back(JsonEncode(new Dictionary({{"env_id", envId}, {"timeperiod_id", objectKey}, {"include_id", includeId}})));

			if (configUpdates) {
				configUpdates->emplace_back(typeName + ":override:include:" + id);
			}
		}

		includeChksms.emplace_back(objectKey);
		includeChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", CalculateCheckSumArray(includes)}})));

		Array::Ptr excludes;
		ConfigObject::Ptr (*getExclude)(const String& name);

		excludes = timeperiod->GetExcludes();
		getExclude = &::GetObjectByName<TimePeriod>;

		Array::Ptr excludeChecksums = new Array();

		ObjectLock excludesLock(excludes);
		ObjectLock excludeChecksumsLock(excludeChecksums);

		excludeChecksums->Reserve(excludes->GetLength());

		auto& excluds (hMSets[m_PrefixConfigObject + typeName + ":override:exclude"]);
		auto& excludeChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":override:exclude"]);

		for (auto exclude : excludes) {
			String excludeId = GetObjectIdentifier((*getExclude)(exclude.Get<String>()));
			excludeChecksums->Add(excludeId);

			String id = CalculateCheckSumArray(new Array({envId, excludeId, objectKey}));
			excluds.emplace_back(id);
			excluds.emplace_back(JsonEncode(new Dictionary({{"env_id", envId}, {"timeperiod_id", objectKey}, {"exclude_id", excludeId}})));

			if (configUpdates) {
				configUpdates->emplace_back(typeName + ":override:exclude:" + id);
			}
		}

		excludeChksms.emplace_back(objectKey);
		excludeChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", CalculateCheckSumArray(excludes)}})));

		return;
	}

	if (type == Zone::TypeInstance) {
		Zone::Ptr zone = static_pointer_cast<Zone>(object);

		Array::Ptr parents(new Array);
		auto parentsRaw (zone->GetAllParentsRaw());

		parents->Reserve(parentsRaw.size());

		auto& parnts (hMSets[m_PrefixConfigObject + typeName + ":parent"]);
		auto& parentChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":parent"]);

		for (auto& parent : parentsRaw) {
			String parentId = GetObjectIdentifier(parent);
			String id = CalculateCheckSumArray(new Array({envId, parentId, objectKey}));
			parnts.emplace_back(id);
			parnts.emplace_back(JsonEncode(new Dictionary({{"zone_id", objectKey}, {"env_id", envId}, {"parent_id", parentId}})));

			if (configUpdates) {
				configUpdates->emplace_back(typeName + ":parent:" + id);
			}

			parents->Add(GetObjectIdentifier(parent));
		}

		parentChksms.emplace_back(objectKey);
		parentChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", HashValue(zone->GetAllParents())}})));

		return;
	}

	if (type == User::TypeInstance) {
		User::Ptr user = static_pointer_cast<User>(object);

		Array::Ptr groups;
		ConfigObject::Ptr (*getGroup)(const String& name);

		groups = user->GetGroups();
		getGroup = &::GetObjectByName<UserGroup>;

		if (groups) {
			ObjectLock groupsLock(groups);
			Array::Ptr groupIds(new Array);

			groupIds->Reserve(groups->GetLength());

			auto& members (hMSets[m_PrefixConfigObject + typeName + ":groupmember"]);
			auto& memberChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":groupmember"]);

			for (auto& group : groups) {
				String groupId = GetObjectIdentifier((*getGroup)(group));
				String id = CalculateCheckSumArray(new Array({envId, groupId, objectKey}));
				members.emplace_back(id);
				members.emplace_back(JsonEncode(new Dictionary({{"user_id", objectKey}, {"env_id", envId}, {"group_id", groupId}})));

				if (configUpdates) {
					configUpdates->emplace_back(typeName + ":groupmember:" + id);
				}

				groupIds->Add(groupId);
			}

			memberChksms.emplace_back(objectKey);
			memberChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", CalculateCheckSumArray(groupIds)}})));
		}

		return;
	}

	if (type == Notification::TypeInstance) {
		Notification::Ptr notification = static_pointer_cast<Notification>(object);

		std::set<User::Ptr> users = notification->GetUsers();
		Array::Ptr userIds = new Array();

		auto usergroups(notification->GetUserGroups());
		Array::Ptr usergroupIds = new Array();

		userIds->Reserve(users.size());

		auto& usrs (hMSets[m_PrefixConfigObject + typeName + ":user"]);
		auto& userChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":user"]);

		for (auto& user : users) {
			String userId = GetObjectIdentifier(user);
			String id = CalculateCheckSumArray(new Array({envId, userId, objectKey}));
			usrs.emplace_back(id);
			usrs.emplace_back(JsonEncode(new Dictionary({{"notification_id", objectKey}, {"env_id", envId}, {"user_id", userId}})));

			if (configUpdates) {
				configUpdates->emplace_back(typeName + ":user:" + id);
			}

			userIds->Add(userId);
		}

		userChksms.emplace_back(objectKey);
		userChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", CalculateCheckSumArray(userIds)}})));

		usergroupIds->Reserve(usergroups.size());

		auto& groups (hMSets[m_PrefixConfigObject + typeName + ":usergroup"]);
		auto& groupChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":usergroup"]);

		for (auto& usergroup : usergroups) {
			String usergroupId = GetObjectIdentifier(usergroup);
			String id = CalculateCheckSumArray(new Array({envId, usergroupId, objectKey}));
			groups.emplace_back(id);
			groups.emplace_back(JsonEncode(new Dictionary({{"notification_id", objectKey}, {"env_id", envId}, {"usergroup_id", usergroupId}})));

			if (configUpdates) {
				configUpdates->emplace_back(typeName + ":usergroup:" + id);
			}

			usergroupIds->Add(usergroupId);
		}

		groupChksms.emplace_back(objectKey);
		groupChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", CalculateCheckSumArray(usergroupIds)}})));

		return;
	}

	if (type == CheckCommand::TypeInstance || type == NotificationCommand::TypeInstance || type == EventCommand::TypeInstance) {
		Command::Ptr command = static_pointer_cast<Command>(object);

		Dictionary::Ptr arguments = command->GetArguments();
		if (arguments) {
			ObjectLock argumentsLock(arguments);
			auto& typeArgs (hMSets[m_PrefixConfigObject + typeName + ":argument"]);
			auto& argChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":argument"]);

			for (auto& kv : arguments) {
				Dictionary::Ptr values;
				if (kv.second.IsObjectType<Dictionary>()) {
					values = kv.second;
					values = values->ShallowClone();
				} else if (kv.second.IsObjectType<Array>()) {
					values = new Dictionary({{"value", JsonEncode(kv.second)}});
				} else {
					values = new Dictionary({{"value", kv.second}});
				}

				values->Set("value", JsonEncode(values->Get("value")));
				values->Set("command_id", objectKey);
				values->Set("argument_key", kv.first);
				values->Set("env_id", envId);

				String id = HashValue(objectKey + kv.first + envId);

				typeArgs.emplace_back(id);
				typeArgs.emplace_back(JsonEncode(values));

				if (configUpdates) {
					configUpdates->emplace_back(typeName + ":argument:" + id);
				}

				argChksms.emplace_back(id);
				argChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", HashValue(kv.second)}})));
			}
		}

		Dictionary::Ptr envvars = command->GetEnv();
		if (envvars) {
			ObjectLock envvarsLock(envvars);
			Array::Ptr envvarIds(new Array);
			auto& typeVars (hMSets[m_PrefixConfigObject + typeName + ":envvar"]);
			auto& varChksms (hMSets[m_PrefixConfigCheckSum + typeName + ":envvar"]);

			envvarIds->Reserve(envvars->GetLength());

			for (auto& kv : envvars) {
				Dictionary::Ptr values;
				if (kv.second.IsObjectType<Dictionary>()) {
					values = kv.second;
					values = values->ShallowClone();
				} else if (kv.second.IsObjectType<Array>()) {
					values = new Dictionary({{"value", JsonEncode(kv.second)}});
				} else {
					values = new Dictionary({{"value", kv.second}});
				}

				values->Set("value", JsonEncode(values->Get("value")));
				values->Set("command_id", objectKey);
				values->Set("envvar_key", kv.first);
				values->Set("env_id", envId);

				String id = HashValue(objectKey + kv.first + envId);

				typeVars.emplace_back(id);
				typeVars.emplace_back(JsonEncode(values));

				if (configUpdates) {
					configUpdates->emplace_back(typeName + ":envvar:" + id);
				}

				varChksms.emplace_back(id);
				varChksms.emplace_back(JsonEncode(new Dictionary({{"checksum", HashValue(kv.second)}})));
			}
		}

		return;
	}
}

void RedisWriter::UpdateState(const Checkable::Ptr& checkable)
{
	Dictionary::Ptr stateAttrs = SerializeState(checkable);

	m_Rcon->FireAndForgetQuery({"HSET", m_PrefixStateObject + GetLowerCaseTypeNameDB(checkable), GetObjectIdentifier(checkable), JsonEncode(stateAttrs)});
}

// Used to update a single object, used for runtime updates
void RedisWriter::SendConfigUpdate(const ConfigObject::Ptr& object, bool runtimeUpdate)
{
	if (!m_Rcon || !m_Rcon->IsConnected())
		return;

	String typeName = GetLowerCaseTypeNameDB(object);

	std::map<String, std::vector<String>> hMSets, publishes;
	std::vector<String> states 							= {"HMSET", m_PrefixStateObject + typeName};

	CreateConfigUpdate(object, typeName, hMSets, publishes, runtimeUpdate);
	Checkable::Ptr checkable = dynamic_pointer_cast<Checkable>(object);
	if (checkable) {
		m_Rcon->FireAndForgetQuery({"HSET", m_PrefixStateObject + typeName,
							  GetObjectIdentifier(checkable), JsonEncode(SerializeState(checkable))});
	}

	std::vector<std::vector<String> > transaction = {{"MULTI"}};

	for (auto& kv : hMSets) {
		if (!kv.second.empty()) {
			kv.second.insert(kv.second.begin(), {"HMSET", kv.first});
			transaction.emplace_back(std::move(kv.second));
		}
	}

	for (auto& kv : publishes) {
		for (auto& message : kv.second) {
			std::vector<String> publish;

			publish.reserve(3);
			publish.emplace_back("PUBLISH");
			publish.emplace_back(kv.first);
			publish.emplace_back(std::move(message));

			transaction.emplace_back(std::move(publish));
		}
	}

	if (transaction.size() > 1) {
		transaction.push_back({"EXEC"});
		m_Rcon->FireAndForgetQueries(std::move(transaction));
	}
}

// Takes object and collects IcingaDB relevant attributes and computes checksums. Returns whether the object is relevant
// for IcingaDB.
bool RedisWriter::PrepareObject(const ConfigObject::Ptr& object, Dictionary::Ptr& attributes, Dictionary::Ptr& checksums)
{
	attributes->Set("name_checksum", CalculateCheckSumString(object->GetName()));
	attributes->Set("env_id", CalculateCheckSumString(GetEnvironment()));
	attributes->Set("name", object->GetName());

	Zone::Ptr ObjectsZone = static_pointer_cast<Zone>(object->GetZone());
	if (ObjectsZone) {
		attributes->Set("zone_id", GetObjectIdentifier(ObjectsZone));
		attributes->Set("zone", ObjectsZone->GetName());
	}

	Type::Ptr type = object->GetReflectionType();

	if (type == Endpoint::TypeInstance) {
		return true;
	}

	if (type == Zone::TypeInstance) {
		Zone::Ptr zone = static_pointer_cast<Zone>(object);

		attributes->Set("is_global", zone->GetGlobal());

		Zone::Ptr parent = zone->GetParent();
		if (parent) {
			attributes->Set("parent_id", GetObjectIdentifier(zone));
		}

		auto parentsRaw (zone->GetAllParentsRaw());
		attributes->Set("depth", parentsRaw.size());

		return true;
	}

	if (type == Host::TypeInstance || type == Service::TypeInstance) {
		Checkable::Ptr checkable = static_pointer_cast<Checkable>(object);

		attributes->Set("checkcommand", checkable->GetCheckCommand()->GetName());
		attributes->Set("max_check_attempts", checkable->GetMaxCheckAttempts());
		attributes->Set("check_timeout", checkable->GetCheckTimeout());
		attributes->Set("check_interval", checkable->GetCheckInterval());
		attributes->Set("check_retry_interval", checkable->GetRetryInterval());
		attributes->Set("active_checks_enabled", checkable->GetEnableActiveChecks());
		attributes->Set("passive_checks_enabled", checkable->GetEnablePassiveChecks());
		attributes->Set("event_handler_enabled", checkable->GetEnableEventHandler());
		attributes->Set("notifications_enabled", checkable->GetEnableNotifications());
		attributes->Set("flapping_enabled", checkable->GetEnableFlapping());
		attributes->Set("flapping_threshold_low", checkable->GetFlappingThresholdLow());
		attributes->Set("flapping_threshold_high", checkable->GetFlappingThresholdHigh());
		attributes->Set("perfdata_enabled", checkable->GetEnablePerfdata());
		attributes->Set("is_volatile", checkable->GetVolatile());
		attributes->Set("notes", checkable->GetNotes());
		attributes->Set("icon_image_alt", checkable->GetIconImageAlt());

		attributes->Set("checkcommand_id", GetObjectIdentifier(checkable->GetCheckCommand()));

		Endpoint::Ptr commandEndpoint = checkable->GetCommandEndpoint();
		if (commandEndpoint) {
			attributes->Set("command_endpoint_id", GetObjectIdentifier(commandEndpoint));
			attributes->Set("command_endpoint", commandEndpoint->GetName());
		}

		TimePeriod::Ptr timePeriod = checkable->GetCheckPeriod();
		if (timePeriod) {
			attributes->Set("check_period_id", GetObjectIdentifier(timePeriod));
			attributes->Set("check_period", timePeriod->GetName());
		}

		EventCommand::Ptr eventCommand = checkable->GetEventCommand();
		if (eventCommand) {
			attributes->Set("eventcommand_id", GetObjectIdentifier(eventCommand));
			attributes->Set("eventcommand", eventCommand->GetName());
		}

		String actionUrl = checkable->GetActionUrl();
		String notesUrl = checkable->GetNotesUrl();
		String iconImage = checkable->GetIconImage();
		if (!actionUrl.IsEmpty())
			attributes->Set("action_url_id", CalculateCheckSumArray(new Array({CalculateCheckSumString(GetEnvironment()), actionUrl})));
		if (!notesUrl.IsEmpty())
			attributes->Set("notes_url_id", CalculateCheckSumArray(new Array({CalculateCheckSumString(GetEnvironment()), notesUrl})));
		if (!iconImage.IsEmpty())
			attributes->Set("icon_image_id", CalculateCheckSumArray(new Array({CalculateCheckSumString(GetEnvironment()), iconImage})));


		Host::Ptr host;
		Service::Ptr service;
		tie(host, service) = GetHostService(checkable);

		if (service) {
			attributes->Set("host_id", GetObjectIdentifier(service->GetHost()));
			attributes->Set("display_name", service->GetDisplayName());

			// Overwrite name here, `object->name` is 'HostName!ServiceName' but we only want the name of the Service
			attributes->Set("name", service->GetShortName());
		} else {
			attributes->Set("display_name", host->GetDisplayName());
			attributes->Set("address", host->GetAddress());
			attributes->Set("address6", host->GetAddress6());
		}

		return true;
	}

	if (type == User::TypeInstance) {
		User::Ptr user = static_pointer_cast<User>(object);

		attributes->Set("display_name", user->GetDisplayName());
		attributes->Set("email", user->GetEmail());
		attributes->Set("pager", user->GetPager());
		attributes->Set("notifications_enabled", user->GetEnableNotifications());
		attributes->Set("states", user->GetStates());
		attributes->Set("types", user->GetTypes());

		if (user->GetPeriod())
			attributes->Set("period_id", GetObjectIdentifier(user->GetPeriod()));

		return true;
	}

	if (type == TimePeriod::TypeInstance) {
		TimePeriod::Ptr timeperiod = static_pointer_cast<TimePeriod>(object);

		attributes->Set("display_name", timeperiod->GetDisplayName());
		attributes->Set("prefer_includes", timeperiod->GetPreferIncludes());
		return true;
	}

	if (type == Notification::TypeInstance) {
		Notification::Ptr notification = static_pointer_cast<Notification>(object);

		Host::Ptr host;
		Service::Ptr service;

		tie(host, service) = GetHostService(notification->GetCheckable());

		attributes->Set("host_id", GetObjectIdentifier(host));
		attributes->Set("command_id", GetObjectIdentifier(notification->GetCommand()));

		if (service)
			attributes->Set("service_id", GetObjectIdentifier(service));

		TimePeriod::Ptr timeperiod = notification->GetPeriod();
		if (timeperiod)
			attributes->Set("period_id", GetObjectIdentifier(timeperiod));

		if (notification->GetTimes()) {
			attributes->Set("times_begin", notification->GetTimes()->Get("begin"));
			attributes->Set("times_end",notification->GetTimes()->Get("end"));
		}

		attributes->Set("notification_interval", notification->GetInterval());
		attributes->Set("states", notification->GetStates());
		attributes->Set("types", notification->GetTypes());

		return true;
	}

	if (type == Comment::TypeInstance) {
		Comment::Ptr comment = static_pointer_cast<Comment>(object);

		attributes->Set("author", comment->GetAuthor());
		attributes->Set("text", comment->GetText());
		attributes->Set("entry_type", comment->GetEntryType());
		attributes->Set("entry_time", comment->GetEntryTime());
		attributes->Set("is_persistent", comment->GetPersistent());
		attributes->Set("expire_time", comment->GetExpireTime());

		Host::Ptr host;
		Service::Ptr service;
		tie(host, service) = GetHostService(comment->GetCheckable());
		if (service)
			attributes->Set("service_id", GetObjectIdentifier(service));
		else
			attributes->Set("host_id", GetObjectIdentifier(host));

		return true;
	}

	if (type == Downtime::TypeInstance) {
		Downtime::Ptr downtime = static_pointer_cast<Downtime>(object);

		attributes->Set("author", downtime->GetAuthor());
		attributes->Set("comment", downtime->GetComment());
		attributes->Set("entry_time", downtime->GetEntryTime());
		attributes->Set("scheduled_start_time", downtime->GetStartTime());
		attributes->Set("scheduled_end_time", downtime->GetEndTime());
		attributes->Set("duration", downtime->GetDuration());
		attributes->Set("is_fixed", downtime->GetFixed());
		attributes->Set("is_in_effect", downtime->IsInEffect());
		if (downtime->IsInEffect())
			attributes->Set("actual_start_time", downtime->GetTriggerTime());

		Host::Ptr host;
		Service::Ptr service;
		tie(host, service) = GetHostService(downtime->GetCheckable());

		if (service) {
			attributes->Set("service_id", GetObjectIdentifier(service));
		} else
			attributes->Set("host_id", GetObjectIdentifier(host));

		return true;
	}

	if (type == UserGroup::TypeInstance) {
		UserGroup::Ptr userGroup = static_pointer_cast<UserGroup>(object);

		attributes->Set("display_name", userGroup->GetDisplayName());

		return true;
	}

	if (type == HostGroup::TypeInstance) {
		HostGroup::Ptr hostGroup = static_pointer_cast<HostGroup>(object);

		attributes->Set("display_name", hostGroup->GetDisplayName());

		return true;
	}

	if (type == ServiceGroup::TypeInstance) {
		ServiceGroup::Ptr serviceGroup = static_pointer_cast<ServiceGroup>(object);

		attributes->Set("display_name", serviceGroup->GetDisplayName());

		return true;
	}

	if (type == CheckCommand::TypeInstance || type == NotificationCommand::TypeInstance || type == EventCommand::TypeInstance) {
		Command::Ptr command = static_pointer_cast<Command>(object);

		attributes->Set("command", JsonEncode(command->GetCommandLine()));
		attributes->Set("timeout", command->GetTimeout());

		return true;
	}

	return false;
}

/* Creates a config update with computed checksums etc.
 * Writes attributes, customVars and checksums into the respective supplied vectors. Adds two values to each vector
 * (if applicable), first the key then the value. To use in a Redis command the command (e.g. HSET) and the key (e.g.
 * icinga:config:object:downtime) need to be prepended. There is nothing to indicate success or failure.
 */
void
RedisWriter::CreateConfigUpdate(const ConfigObject::Ptr& object, const String typeName, std::map<String, std::vector<String>>& hMSets,
								std::map<String, std::vector<String>>& publishes, bool runtimeUpdate)
{
	/* TODO: This isn't essentially correct as we don't keep track of config objects ourselves. This would avoid duplicated config updates at startup.
	if (!runtimeUpdate && m_ConfigDumpInProgress)
		return;
	*/

	if (m_Rcon == nullptr)
		return;

	Dictionary::Ptr attr = new Dictionary;
	Dictionary::Ptr chksm = new Dictionary;

	if (!PrepareObject(object, attr, chksm))
		return;

	InsertObjectDependencies(object, typeName, hMSets, publishes, runtimeUpdate);

	String objectKey = GetObjectIdentifier(object);
	auto& attrs (hMSets[m_PrefixConfigObject + typeName]);
	auto& chksms (hMSets[m_PrefixConfigCheckSum + typeName]);

	attrs.emplace_back(objectKey);
	attrs.emplace_back(JsonEncode(attr));

	chksms.emplace_back(objectKey);
	chksms.emplace_back(JsonEncode(new Dictionary({{"checksum", HashValue(attr)}})));

	/* Send an update event to subscribers. */
	if (runtimeUpdate) {
		publishes["icinga:config:update"].emplace_back(typeName + ":" + objectKey);
	}
}

void RedisWriter::SendConfigDelete(const ConfigObject::Ptr& object)
{
	String typeName = object->GetReflectionType()->GetName().ToLower();
	String objectKey = GetObjectIdentifier(object);

	m_Rcon->FireAndForgetQueries({
								   {"HDEL",    m_PrefixConfigObject + typeName, objectKey},
								   {"DEL",     m_PrefixStateObject + typeName + ":" + objectKey},
								   {"PUBLISH", "icinga:config:delete", typeName + ":" + objectKey}
						   });
}

void RedisWriter::SendStatusUpdate(const ConfigObject::Ptr& object)
{
	if (!m_Rcon || !m_Rcon->IsConnected())
		return;

	Checkable::Ptr checkable = dynamic_pointer_cast<Checkable>(object);
	if (!checkable)
		return;

	Host::Ptr host;
	Service::Ptr service;

	tie(host, service) = GetHostService(checkable);

	String streamname;
	if (service)
		streamname = "icinga:state:stream:service";
	else
		streamname = "icinga:state:stream:host";

	Dictionary::Ptr objectAttrs = SerializeState(checkable);

	std::vector<String> streamadd({"XADD", streamname, "*"});
	ObjectLock olock(objectAttrs);
	for (const Dictionary::Pair& kv : objectAttrs) {
		streamadd.emplace_back(kv.first);
		streamadd.emplace_back(Utility::ValidateUTF8(kv.second));
	}

	m_Rcon->FireAndForgetQuery(std::move(streamadd));
}

Dictionary::Ptr RedisWriter::SerializeState(const Checkable::Ptr& checkable)
{
	Dictionary::Ptr attrs = new Dictionary();

	Host::Ptr host;
	Service::Ptr service;

	tie(host, service) = GetHostService(checkable);

	attrs->Set("id", GetObjectIdentifier(checkable));;
	attrs->Set("env_id", CalculateCheckSumString(GetEnvironment()));
	attrs->Set("state_type", checkable->GetStateType());

	// TODO: last_hard/soft_state should be "previous".
	if (service) {
		attrs->Set("state", service->GetState());
		attrs->Set("last_soft_state", service->GetState());
		attrs->Set("last_hard_state", service->GetLastHardState());
		attrs->Set("severity", service->GetSeverity());
	} else {
		attrs->Set("state", host->GetState());
		attrs->Set("last_soft_state", host->GetState());
		attrs->Set("last_hard_state", host->GetLastHardState());
		attrs->Set("severity", host->GetSeverity());
	}

	attrs->Set("check_attempt", checkable->GetCheckAttempt());

	attrs->Set("is_active", checkable->IsActive());

	CheckResult::Ptr cr = checkable->GetLastCheckResult();

	if (cr) {
		String rawOutput = cr->GetOutput();
		if (!rawOutput.IsEmpty()) {
			size_t lineBreak = rawOutput.Find("\n");
			String output = rawOutput.SubStr(0, lineBreak);
			if (!output.IsEmpty())
				attrs->Set("output", rawOutput.SubStr(0, lineBreak));

			if (lineBreak > 0 && lineBreak != String::NPos) {
				String longOutput = rawOutput.SubStr(lineBreak+1, rawOutput.GetLength());
				if (!longOutput.IsEmpty())
					attrs->Set("long_output", longOutput);
			}
		}

		String perfData = PluginUtility::FormatPerfdata(cr->GetPerformanceData());
		if (!perfData.IsEmpty())
			attrs->Set("performance_data", perfData);

		if (!cr->GetCommand().IsEmpty())
			attrs->Set("commandline", FormatCommandLine(cr->GetCommand()));
		attrs->Set("execution_time", cr->CalculateExecutionTime());
		attrs->Set("latency", cr->CalculateLatency());
	}

	bool isProblem = !checkable->IsStateOK(checkable->GetStateRaw());
	attrs->Set("is_problem", isProblem);
	attrs->Set("is_handled", isProblem && (checkable->IsInDowntime() || checkable->IsAcknowledged()));
	attrs->Set("is_reachable", checkable->IsReachable());
	attrs->Set("is_flapping", checkable->IsFlapping());

	attrs->Set("is_acknowledged", checkable->IsAcknowledged());
	if (checkable->IsAcknowledged()) {
		Timestamp entry = 0;
		Comment::Ptr AckComment;
		for (const Comment::Ptr& c : checkable->GetComments()) {
			if (c->GetEntryType() == CommentAcknowledgement) {
				if (c->GetEntryTime() > entry) {
					entry = c->GetEntryTime();
					AckComment = c;
				}
			}
		}
		if (AckComment != nullptr) {
			attrs->Set("acknowledgement_comment_id", GetObjectIdentifier(AckComment));
		}
	}

	attrs->Set("in_downtime", checkable->IsInDowntime());

	if (checkable->GetCheckTimeout().IsEmpty())
		attrs->Set("check_timeout",checkable->GetCheckCommand()->GetTimeout());
	else
		attrs->Set("check_timeout", checkable->GetCheckTimeout());

	attrs->Set("last_update", Utility::GetTime());
	attrs->Set("last_state_change", checkable->GetLastStateChange());
	attrs->Set("next_check", checkable->GetNextCheck());

	return attrs;
}

std::vector<String>
RedisWriter::UpdateObjectAttrs(const ConfigObject::Ptr& object, int fieldType,
							   const String& typeNameOverride)
{
	Type::Ptr type = object->GetReflectionType();
	Dictionary::Ptr attrs(new Dictionary);

	for (int fid = 0; fid < type->GetFieldCount(); fid++) {
		Field field = type->GetFieldInfo(fid);

		if ((field.Attributes & fieldType) == 0)
			continue;

		Value val = object->GetField(fid);

		/* hide attributes which shouldn't be user-visible */
		if (field.Attributes & FANoUserView)
			continue;

		/* hide internal navigation fields */
		if (field.Attributes & FANavigation && !(field.Attributes & (FAConfig | FAState)))
			continue;

		attrs->Set(field.Name, Serialize(val));
	}

	/* Downtimes require in_effect, which is not an attribute */
	Downtime::Ptr downtime = dynamic_pointer_cast<Downtime>(object);
	if (downtime) {
		attrs->Set("in_effect", Serialize(downtime->IsInEffect()));
		attrs->Set("trigger_time", Serialize(downtime->GetTriggerTime()));
	}


	/* Use the name checksum as unique key. */
	String typeName = type->GetName().ToLower();
	if (!typeNameOverride.IsEmpty())
		typeName = typeNameOverride.ToLower();

	return {GetObjectIdentifier(object), JsonEncode(attrs)};
	//m_Rcon->FireAndForgetQuery({"HSET", keyPrefix + typeName, GetObjectIdentifier(object), JsonEncode(attrs)});
}

void RedisWriter::StateChangeHandler(const ConfigObject::Ptr &object)
{
	Type::Ptr type = object->GetReflectionType();

	for (const RedisWriter::Ptr& rw : ConfigType::GetObjectsByType<RedisWriter>()) {
		rw->m_WorkQueue.Enqueue([rw, object]() { rw->SendStatusUpdate(object); });
	}
}

void RedisWriter::VersionChangedHandler(const ConfigObject::Ptr& object)
{
	Type::Ptr type = object->GetReflectionType();

	if (object->IsActive()) {
		// Create or update the object config
		for (const RedisWriter::Ptr& rw : ConfigType::GetObjectsByType<RedisWriter>()) {
			if (rw)
				rw->m_WorkQueue.Enqueue([rw, object]() { rw->SendConfigUpdate(object, true); });
		}
	} else if (!object->IsActive() &&
			   object->GetExtension("ConfigObjectDeleted")) { // same as in apilistener-configsync.cpp
		// Delete object config
		for (const RedisWriter::Ptr& rw : ConfigType::GetObjectsByType<RedisWriter>()) {
			if (rw)
				rw->m_WorkQueue.Enqueue([rw, object]() { rw->SendConfigDelete(object); });
		}
	}
}

void RedisWriter::DowntimeChangedHandler(const Downtime::Ptr& downtime)
{
	StateChangeHandler(downtime->GetCheckable());
}
