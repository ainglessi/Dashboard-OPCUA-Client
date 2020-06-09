#include "DashboardMachineObserver.hpp"
#include <easylogging++.h>
#include "Exceptions/MachineInvalidException.hpp"
#include <Exceptions/OpcUaException.hpp>
#include <TypeDefinition/UmatiTypeNodeIds.hpp>

namespace Umati {
	namespace MachineObserver {

		DashboardMachineObserver::DashboardMachineObserver(
			std::shared_ptr<Dashboard::IDashboardDataClient> pDataClient,
			std::shared_ptr<Umati::Dashboard::IPublisher> pPublisher
		) : MachineObserver(pDataClient),
			m_pPublisher(pPublisher) {
			startUpdateMachineThread();
		}

		DashboardMachineObserver::~DashboardMachineObserver()
		{
			stopMachineUpdateThread();
		}

		void DashboardMachineObserver::PublishAll()
		{
			std::unique_lock<decltype(m_dashboardClients_mutex)> ul(m_dashboardClients_mutex);
			for (const auto& pDashClient : m_dashboardClients)
			{
				pDashClient.second->Publish();
			}

            m_publishMachinesOnline = m_publishMachinesOnline + 1;
			if (m_publishMachinesOnline % 5 == 0)
			{
				this->publishMachinesList();
			}
		}

		void DashboardMachineObserver::startUpdateMachineThread()
		{
			if (m_running)
			{
				LOG(INFO) << "Machine update thread already running";
				return;
			}

			auto func = [this]()
			{
				int cnt = 0;
				while (this->m_running)
				{
					if ((cnt % 10) == 0)
					{
						this->UpdateMachines();
					}

					++cnt;
					std::this_thread::sleep_for(std::chrono::seconds(1));
				}
			};
			m_running = true;
			m_updateMachineThread = std::thread(func);
		}

		void DashboardMachineObserver::stopMachineUpdateThread()
		{
			m_running = false;
			if (m_updateMachineThread.joinable())
			{
				m_updateMachineThread.join();
			}
		}

		void DashboardMachineObserver::publishMachinesList()
		{
			std::map<std::string, nlohmann::json> publishData;
			for (auto &machineOnline : m_onlineMachines)
			{
				std::string fair = "offsite";
                std::string manufacturer;
                std::string machine_name;
                std::vector<ModelOpcUa::QualifiedName_t> identifierIds;


                ModelOpcUa::NodeId_t type = Dashboard::TypeDefinition::NodeIds::MachineToolIdentificationType;// todo ! change
                int namespaceIndex = m_pDataClient->m_uriToIndexCache[machineOnline.first.Uri];
                std::list<ModelOpcUa::BrowseResult_t> identification = m_pDataClient->Browse(machineOnline.first, ModelOpcUa::NodeId_t{"", std::to_string(OpcUaId_HasComponent)}, type);
                if(!identification.empty()) {

                    UaReferenceDescriptions referenceDescriptions;
                    std::vector<nlohmann::json> identificationListValues;
                    browseIdentificationValues(identification, namespaceIndex, referenceDescriptions, identificationListValues);
                    // todo ! make more generic
                }

				auto findFairListIterator = publishData.find(fair);
				if (findFairListIterator == publishData.end())
				{
					publishData.insert(std::make_pair(fair, nlohmann::json::array()));
				}
				auto fairList = publishData.find(fair);

//                if(!heyJson.empty()) {
//                    fairList->second.push_back("machineIdentification");
//                }
			}

			for (const auto& fairList : publishData) {
				std::stringstream stream;
				m_pPublisher->Publish("/umati/emo/machineList", fairList.second.dump(0));
			}
		}

		void DashboardMachineObserver::addMachine(ModelOpcUa::BrowseResult_t machine)
		{
			try {
				LOG(INFO) << "New Machine: " << machine.BrowseName.Name << " NodeId:" << static_cast<std::string>(machine.NodeId);

				auto pDashClient = std::make_shared<Umati::Dashboard::DashboardClient>(m_pDataClient, m_pPublisher);

				MachineInformation_t machineInformation;
				machineInformation.NamespaceURI = machine.NodeId.Uri;
				machineInformation.StartNodeId  = machine.NodeId;
				machineInformation.Fair = "offsite";

                UaReferenceDescriptions machineComponentsReferenceDescriptions;
                UaReferenceDescriptions singleComponentReferenceDescriptions;
                auto startFromMachineNodeId = UaNodeId::fromXmlString(UaString(machine.NodeId.Id.c_str()));
                uint namespaceIdx = m_pDataClient->m_uriToIndexCache[machine.NodeId.Uri];
                startFromMachineNodeId.setNamespaceIndex(namespaceIdx);

                UaClientSdk::BrowseContext browseContext;
                browseContext.referenceTypeId = OpcUaId_HierarchicalReferences;
                browseContext.browseDirection = OpcUa_BrowseDirection_Forward;
                browseContext.includeSubtype = OpcUa_True;
                browseContext.maxReferencesToReturn = 0;
                browseContext.nodeClassMask = 0; // ALL
                browseContext.resultMask = OpcUa_BrowseResultMask_All;

                m_pDataClient->browseUnderStartNode(startFromMachineNodeId, machineComponentsReferenceDescriptions, browseContext);

                std::shared_ptr<ModelOpcUa::StructureNode> p_type;

                ModelOpcUa::StructureNode type = m_pDataClient->m_typeMap->find("MachineToolType")->second;
                p_type = std::make_shared<ModelOpcUa::StructureNode>(type);

                pDashClient->addDataSet(
                        {machineInformation.NamespaceURI, machine.NodeId.Id},
                        p_type,
                        "/umati/emo/dataSetOfMachineTopic"
                );
                // todo read identification, store string in machineList

				LOG(INFO) << "Read model finished";

				{
					std::unique_lock<decltype(m_dashboardClients_mutex)> ul(m_dashboardClients_mutex);
					m_dashboardClients.insert(std::make_pair(machine.NodeId, pDashClient));
					m_onlineMachines.insert(std::make_pair(machine.NodeId, machineInformation));
				}
			}
			catch (const Umati::Exceptions::OpcUaException &ex)
			{
				LOG(ERROR) << "Could not add Machine " << machine.BrowseName.Name
					<< " NodeId:" << static_cast<std::string>(machine.NodeId) <<
					"OpcUa Error: " << ex.what();

				throw Exceptions::MachineInvalidException(static_cast<std::string>(machine.NodeId));
			}
		}

		void DashboardMachineObserver::removeMachine(ModelOpcUa::BrowseResult_t machine)
		{
			std::unique_lock<decltype(m_dashboardClients_mutex)> ul(m_dashboardClients_mutex);
			LOG(INFO) << "Remove Machine: " << machine.BrowseName.Name << " NodeId:" << static_cast<std::string>(machine.NodeId);
			auto it = m_dashboardClients.find(machine.NodeId);
			if (it != m_dashboardClients.end())
			{
				m_dashboardClients.erase(it); // todo or does it need to be it++ ?
			}
			else
			{
				LOG(INFO) << "Machine not known: '" << static_cast<std::string>(machine.NodeId) << "'";
			}

			auto itOnlineMachines = m_onlineMachines.find(machine.NodeId);
			if (itOnlineMachines != m_onlineMachines.end())
			{
			    LOG(INFO) << "Erasing online machine";
				m_onlineMachines.erase(itOnlineMachines); // todo or dies it need to be itOnlineMachines++?
                LOG(INFO) << "Online machine erased";
            }
			else
			{
				LOG(INFO) << "Machine was not online: '" << static_cast<std::string>(machine.NodeId) << "'";
			}
		}

		bool DashboardMachineObserver::isOnline(const ModelOpcUa::BrowseResult_t& machine, const ModelOpcUa::NodeId_t& type)
		{
            std::vector<ModelOpcUa::QualifiedName_t> identifierIds;
            std::list<ModelOpcUa::BrowseResult_t> identification = m_pDataClient->Browse(machine.NodeId, ModelOpcUa::NodeId_t{"", std::to_string(OpcUaId_HasComponent)} , type);
            if(!identification.empty()) {
                UaReferenceDescriptions referenceDescriptions;
                std::vector<nlohmann::json> identificationListValues;
                int namespaceIndex = m_pDataClient->m_uriToIndexCache[machine.NodeId.Uri];
                browseIdentificationValues(identification, namespaceIndex, referenceDescriptions, identificationListValues);
                if(!identificationListValues.empty()) {
                    return true;
                }
            }
			return false;
		}

        void DashboardMachineObserver::browseIdentificationValues(std::list<ModelOpcUa::BrowseResult_t> &identification,
                                                                  int namespaceIndex,
                                                                  UaReferenceDescriptions &referenceDescriptions,
                                                                  std::vector<nlohmann::json> &identificationListValues) const {

            auto startNodeId = UaNodeId::fromXmlString(UaString(identification.front().NodeId.Id.c_str()));
            startNodeId.setNamespaceIndex(namespaceIndex);
            m_pDataClient->browseUnderStartNode(startNodeId, referenceDescriptions);

            std::list<ModelOpcUa::NodeId_t> identificationNodes;
            for (OpcUa_UInt32 i = 0; i < referenceDescriptions.length(); i++) {
                ModelOpcUa::BrowseResult_t browseResult = m_pDataClient->ReferenceDescriptionToBrowseResult(referenceDescriptions[i]);
                identificationNodes.emplace_back(browseResult.NodeId);
            }
            identificationListValues= m_pDataClient->readValues(identificationNodes);
        }
	}
}
