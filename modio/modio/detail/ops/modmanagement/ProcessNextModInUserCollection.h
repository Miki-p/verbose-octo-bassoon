/* 
 *  Copyright (C) 2021 mod.io Pty Ltd. <https://mod.io>
 *  
 *  This file is part of the mod.io SDK.
 *  
 *  Distributed under the MIT License. (See accompanying file LICENSE or 
 *   view online at <https://github.com/modio/modio-sdk/blob/main/LICENSE>)
 *   
 */

#pragma once
#include "modio/core/ModioLogger.h"
#include "modio/core/ModioModCollectionEntry.h"
#include "modio/core/ModioServices.h"
#include "modio/detail/AsioWrapper.h"
#include "modio/detail/ModioSDKSessionData.h"
#include "modio/detail/ops/SaveModCollectionToStorage.h"
#include "modio/detail/ops/modmanagement/InstallOrUpdateMod.h"
#include "modio/detail/ops/modmanagement/UninstallMod.h"
#include "modio/userdata/ModioUserDataService.h"
#include <asio/yield.hpp>
namespace Modio
{
	namespace Detail
	{
		/// @brief Internal operation. Searches the user's mod collection for the next mod marked as requiring
		/// installation, update, or uninstallation, then performs that operation
		class ProcessNextModInUserCollection
		{
		public:
			template<typename CoroType>
			void operator()(CoroType& Self, Modio::ErrorCode ec = {})
			{
				reenter(CoroutineState)
				{
					{
						EntryToProcess = nullptr;
						// Check for pending uninstallations regardless of user
						for (auto ModEntry : Modio::Detail::SDKSessionData::GetSystemModCollection().Entries())
						{
							if (ModEntry.second->GetModState() == Modio::ModState::UninstallPending)
							{
								if (ModEntry.second->ShouldRetry())
								{
									EntryToProcess = ModEntry.second;
								}
							}
						}

						// If no pending uninstallations, check for this users installs or updates
						if (!EntryToProcess)
						{
							Modio::ModCollection UserModCollection =
								Modio::Detail::SDKSessionData::FilterSystemModCollectionByUserSubscriptions();
							for (auto ModEntry : UserModCollection.Entries())
							{
								Modio::ModState CurrentState = ModEntry.second->GetModState();
								if (CurrentState == Modio::ModState::InstallationPending ||
									CurrentState == Modio::ModState::UpdatePending)
								{
									if (ModEntry.second->ShouldRetry())
									{
										EntryToProcess = ModEntry.second;
									}
								}
							}
						}
					}
					if (EntryToProcess == nullptr)
					{
						Self.complete({});
						return;
					}
					if (EntryToProcess->GetModState() == Modio::ModState::InstallationPending ||
						EntryToProcess->GetModState() == Modio::ModState::UpdatePending)
					{
						// Does this need to be a separate operation or could we provide a parameter to specify
						// we only want to update if it's already installed or something?
						yield Modio::Detail::InstallOrUpdateModAsync(EntryToProcess->GetID(), std::move(Self));
						Modio::Detail::SDKSessionData::GetModManagementEventLog().AddEntry(Modio::ModManagementEvent {
							EntryToProcess->GetID(),
							EntryToProcess->GetModState() == Modio::ModState::InstallationPending
								? Modio::ModManagementEvent::EventType::Installed
								: Modio::ModManagementEvent::EventType::Updated,
							ec});
						if (ec)
						{
							if (Modio::ErrorCodeMatches(ec, Modio::ErrorConditionTypes::NetworkError) &&
								ec != Modio::ModManagementError::InstallOrUpdateCancelled)
							{
								EntryToProcess->MarkModNoRetry();
							}
							if (Modio::ErrorCodeMatches(ec, Modio::ErrorConditionTypes::ModInstallDeferredError))
							{
								EntryToProcess->MarkModNoRetry();
							}
							if (Modio::ErrorCodeMatches(ec, Modio::ApiError::ExpiredOrRevokedAccessToken))
							{
								EntryToProcess->MarkModNoRetry();
							}
							Self.complete(ec);
							return;
						}
						else
						{
							yield Modio::Detail::SaveModCollectionToStorageAsync(std::move(Self));

							Self.complete({});
							return;
						}
					}
					else if (EntryToProcess->GetModState() == Modio::ModState::UninstallPending)
					{
						yield Modio::Detail::UninstallModAsync(EntryToProcess->GetID(), std::move(Self));
						Modio::Detail::SDKSessionData::GetModManagementEventLog().AddEntry(Modio::ModManagementEvent {
							EntryToProcess->GetID(), Modio::ModManagementEvent::EventType::Uninstalled, ec});
						if (ec)
						{
							if (Modio::ErrorCodeMatches(Modio::ErrorCode(0x91, std::system_category()),
														Modio::ErrorConditionTypes::ModDeleteDeferredError))
							{}
							EntryToProcess->MarkModNoRetry();
							Self.complete(ec);
							return;
						}
						else
						{
							yield Modio::Detail::SaveModCollectionToStorageAsync(std::move(Self));

							Self.complete({});
							return;
						}
					}
				}
			}

		private:
			asio::coroutine CoroutineState;
			std::shared_ptr<Modio::ModCollectionEntry> EntryToProcess;
		};

		template<typename ProcessNextCallback>
		auto ProcessNextModInUserCollectionAsync(ProcessNextCallback&& OnProcessComplete)
		{
			return asio::async_compose<ProcessNextCallback, void(Modio::ErrorCode)>(
				ProcessNextModInUserCollection(), OnProcessComplete,
				Modio::Detail::Services::GetGlobalContext().get_executor());
		}
	} // namespace Detail
} // namespace Modio

#include <asio/unyield.hpp>