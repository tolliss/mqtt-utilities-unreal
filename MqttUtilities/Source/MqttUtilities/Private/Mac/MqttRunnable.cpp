// Copyright (c) 2019 Nineva Studios

#include "MqttRunnable.h"

#include "Async/Async.h"
#include "Misc/Paths.h"
#include "MqttClient.h"
#include "MqttClientImpl.h"
#include "MqttUtilitiesSettings.h"
#include "MqttUtilitiesModule.h"

FMqttRunnable::FMqttRunnable(UMqttClient* mqttClient)
	: FRunnable()
	, TaskQueue(new std::queue<FMqttTaskPtr>())
	, TaskQueueLock(new FCriticalSection())
	, client(mqttClient)
{
}

FMqttRunnable::~FMqttRunnable()
{
	if (TaskQueueLock != nullptr)
	{
		delete TaskQueueLock;
		TaskQueueLock = nullptr;
	}

	if (TaskQueue != nullptr)
	{
		delete TaskQueue;
		TaskQueue = nullptr;
	}
}

bool FMqttRunnable::Init()
{
	bKeepRunning = true;
	return true;
}

void FMqttRunnable::Stop()
{
	StopRunning();
}

uint32 FMqttRunnable::Run()
{
	mosqpp::lib_init();

	MqttClientImpl connection(ClientId.c_str());

	connection.max_inflight_messages_set(0);
	connection.Task = this;

	int returnCode = 0;

	if (bIsSecureConnection)
	{
		UMqttUtilitiesSettings* Settings = FMqttUtilitiesModule::Get().GetSettings();
		FString PemCertBundlePath = FPaths::ProjectContentDir();
		PemCertBundlePath /= "Certificates";
		PemCertBundlePath /= Settings->DesktopCertificateName;
		PemCertBundlePath = IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*PemCertBundlePath);

		if (IFileManager::Get().FileExists(*PemCertBundlePath))
		{
			returnCode = connection.tls_set(TCHAR_TO_ANSI(*PemCertBundlePath));
			if (returnCode != MOSQ_ERR_SUCCESS)
			{
				UE_LOG(LogTemp, Error, TEXT("MQTT => TLS error: %s"), ANSI_TO_TCHAR(mosquitto_strerror(returnCode)));
				OnError(returnCode, FString(ANSI_TO_TCHAR(mosquitto_strerror(returnCode))));
				bKeepRunning = false;
			}
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("MQTT => Could not find certificate bundle at: %s"), *PemCertBundlePath);
		}
	}

	if (!Username.empty())
	{
		connection.username_pw_set(Username.c_str(), Password.c_str());
	}

	returnCode = connection.connect(Host.c_str(), Port, 10);

	if (returnCode != MOSQ_ERR_SUCCESS)
	{
		UE_LOG(LogTemp, Error, TEXT("MQTT => Connection error: %s"), ANSI_TO_TCHAR(mosquitto_strerror(returnCode)));
		OnError(returnCode, FString(ANSI_TO_TCHAR(mosquitto_strerror(returnCode))));
		bKeepRunning = false;
	}

	while (bKeepRunning)
	{
		TaskQueueLock->Lock();

		while (TaskQueue != nullptr)
		{
			if (TaskQueue->empty())
			{
				break;
			}

			FMqttTaskPtr task = TaskQueue->front();
			TaskQueue->pop();

			switch (task->type)
			{
				case MqttTaskType::Subscribe:
				{
					auto taskSubscribe = StaticCastSharedPtr<FMqttSubscribeTask>(task);
					returnCode = connection.subscribe(NULL, taskSubscribe->sub, taskSubscribe->qos);
					break;
				}
				case MqttTaskType::Unsubscribe:
				{
					auto taskUnsubscribe = StaticCastSharedPtr<FMqttUnsubscribeTask>(task);
					returnCode = connection.unsubscribe(NULL, taskUnsubscribe->sub);
					break;
				}
				case MqttTaskType::Publish:
				{
					auto taskPublish = StaticCastSharedPtr<FMqttPublishTask>(task);
					returnCode = connection.publish(NULL, taskPublish->topic, taskPublish->payloadlen, taskPublish->payload, taskPublish->qos, taskPublish->retain);
					break;
				}
			}

			if (returnCode != MOSQ_ERR_SUCCESS)
			{
				UE_LOG(LogTemp, Error, TEXT("MQTT => Output error: %s"), ANSI_TO_TCHAR(mosquitto_strerror(returnCode)));
				OnError(returnCode, FString(ANSI_TO_TCHAR(mosquitto_strerror(returnCode))));
			}
		}

		TaskQueueLock->Unlock();

		returnCode = connection.loop();

		if (returnCode != MOSQ_ERR_SUCCESS)
		{
			UE_LOG(LogTemp, Error, TEXT("MQTT => Connection error: %s"), ANSI_TO_TCHAR(mosquitto_strerror(returnCode)));

			if (returnCode == MOSQ_ERR_CONN_REFUSED || returnCode == MOSQ_ERR_TLS)
			{
				OnError(returnCode, FString(ANSI_TO_TCHAR(mosquitto_strerror(returnCode))));
				bKeepRunning = false;
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("MQTT => Trying to reconnect"));
				connection.reconnect();
			}
		}
	}

	returnCode = connection.disconnect();

	if (returnCode != MOSQ_ERR_SUCCESS)
	{
		UE_LOG(LogTemp, Error, TEXT("MQTT => %s"), ANSI_TO_TCHAR(mosquitto_strerror(returnCode)));
	}

	delete TaskQueue;
	delete TaskQueueLock;

	TaskQueue = nullptr;
	TaskQueueLock = nullptr;

	return 0;
}

void FMqttRunnable::StopRunning()
{
	bKeepRunning = false;
}

bool FMqttRunnable::IsAlive() const
{
	return bKeepRunning;
}

void FMqttRunnable::PushTask(FMqttTaskPtr task)
{
	TaskQueueLock->Lock();

	if (TaskQueue != nullptr)
	{
		TaskQueue->push(task);
	}

	TaskQueueLock->Unlock();
}

void FMqttRunnable::OnConnect()
{
	AsyncTask(ENamedThreads::GameThread, [=]() {
		client->OnConnectDelegate.ExecuteIfBound();
	});
}

void FMqttRunnable::OnDisconnect()
{
	AsyncTask(ENamedThreads::GameThread, [=]() {
		client->OnDisconnectDelegate.ExecuteIfBound();
	});
}

void FMqttRunnable::OnPublished(int mid)
{
	AsyncTask(ENamedThreads::GameThread, [=]() {
		client->OnPublishDelegate.ExecuteIfBound(mid);
	});
}

void FMqttRunnable::OnMessage(FMqttMessage message)
{
	AsyncTask(ENamedThreads::GameThread, [=]() {
		client->OnMessageDelegate.ExecuteIfBound(message);
	});
}

void FMqttRunnable::OnSubscribe(int mid, const TArray<int> qos)
{
	AsyncTask(ENamedThreads::GameThread, [=]() {
		client->OnSubscribeDelegate.ExecuteIfBound(mid, qos);
	});
}

void FMqttRunnable::OnUnsubscribe(int mid)
{
	AsyncTask(ENamedThreads::GameThread, [=]() {
		client->OnUnsubscribeDelegate.ExecuteIfBound(mid);
	});
}

void FMqttRunnable::OnError(int errCode, FString message)
{
	AsyncTask(ENamedThreads::GameThread, [=]() {
		client->OnErrorDelegate.ExecuteIfBound(errCode, message);
	});
}
