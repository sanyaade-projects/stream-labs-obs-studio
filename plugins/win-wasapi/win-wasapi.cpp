#include "enum-wasapi.hpp"

#include <obs-module.h>
#include <obs.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <util/windows/HRError.hpp>
#include <util/windows/ComPtr.hpp>
#include <util/windows/WinHandle.hpp>
#include <util/windows/CoTaskMemPtr.hpp>
#include <util/windows/win-version.h>
#include <util/windows/window-helpers.h>
#include <util/threading.h>
#include <util/util_uint64.h>

#include <atomic>
#include <cinttypes>

#include <audioclientactivationparams.h>
#include <avrt.h>
#include <RTWorkQ.h>
#include <psapi.h>
#include <wrl/implements.h>
#include "win-wasapi-app.hpp"

using namespace std;

#define OPT_DEVICE_ID "device_id"
#define OPT_USE_DEVICE_TIMING "use_device_timing"
#define OPT_WINDOW "device_id"
#define OPT_PRIORITY "priority"

static void GetWASAPIDefaults(obs_data_t *settings);

#define OBS_KSAUDIO_SPEAKER_4POINT1 \
	(KSAUDIO_SPEAKER_SURROUND | SPEAKER_LOW_FREQUENCY)

typedef HRESULT(STDAPICALLTYPE *PFN_ActivateAudioInterfaceAsync)(
	LPCWSTR, REFIID, PROPVARIANT *,
	IActivateAudioInterfaceCompletionHandler *,
	IActivateAudioInterfaceAsyncOperation **);

typedef HRESULT(STDAPICALLTYPE *PFN_RtwqUnlockWorkQueue)(DWORD);
typedef HRESULT(STDAPICALLTYPE *PFN_RtwqLockSharedWorkQueue)(PCWSTR usageClass,
							     LONG basePriority,
							     DWORD *taskId,
							     DWORD *id);
typedef HRESULT(STDAPICALLTYPE *PFN_RtwqCreateAsyncResult)(IUnknown *,
							   IRtwqAsyncCallback *,
							   IUnknown *,
							   IRtwqAsyncResult **);
typedef HRESULT(STDAPICALLTYPE *PFN_RtwqPutWorkItem)(DWORD, LONG,
						     IRtwqAsyncResult *);
typedef HRESULT(STDAPICALLTYPE *PFN_RtwqPutWaitingWorkItem)(HANDLE, LONG,
							    IRtwqAsyncResult *,
							    RTWQWORKITEM_KEY *);

class WASAPIActivateAudioInterfaceCompletionHandler
	: public Microsoft::WRL::RuntimeClass<
		  Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
		  Microsoft::WRL::FtmBase,
		  IActivateAudioInterfaceCompletionHandler> {
	IUnknown *unknown;
	HRESULT activationResult;
	WinHandle activationSignal;

public:
	WASAPIActivateAudioInterfaceCompletionHandler();
	HRESULT GetActivateResult(IAudioClient **client);

private:
	virtual HRESULT STDMETHODCALLTYPE ActivateCompleted(
		IActivateAudioInterfaceAsyncOperation *activateOperation)
		override final;
};

WASAPIActivateAudioInterfaceCompletionHandler::
	WASAPIActivateAudioInterfaceCompletionHandler()
{
	activationSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!activationSignal.Valid())
		throw "Could not create receive signal";
}

HRESULT
WASAPIActivateAudioInterfaceCompletionHandler::GetActivateResult(
	IAudioClient **client)
{
	WaitForSingleObject(activationSignal, INFINITE);
	*client = static_cast<IAudioClient *>(unknown);
	return activationResult;
}

HRESULT
WASAPIActivateAudioInterfaceCompletionHandler::ActivateCompleted(
	IActivateAudioInterfaceAsyncOperation *activateOperation)
{
	HRESULT hr, hr_activate;
	hr = activateOperation->GetActivateResult(&hr_activate, &unknown);
	hr = SUCCEEDED(hr) ? hr_activate : hr;
	activationResult = hr;

	SetEvent(activationSignal);
	return hr;
}

enum class SourceType {
	Input,
	DeviceOutput,
	ProcessOutput,
};

class WASAPISource {
	FILE *temp_file = NULL;

	ComPtr<IMMNotificationClient> notify;
	ComPtr<IMMDeviceEnumerator> enumerator;
	ComPtr<IAudioClient> client;
	ComPtr<IAudioCaptureClient> capture;

	static const int MAX_RETRY_INIT_DEVICE_COUNTER = 3;

	obs_source_t *source;
	wstring default_id;
	string device_id;
	string device_name;
	WinModule mmdevapi_module;
	PFN_ActivateAudioInterfaceAsync activate_audio_interface_async = NULL;
	PFN_RtwqUnlockWorkQueue rtwq_unlock_work_queue = NULL;
	PFN_RtwqLockSharedWorkQueue rtwq_lock_shared_work_queue = NULL;
	PFN_RtwqCreateAsyncResult rtwq_create_async_result = NULL;
	PFN_RtwqPutWorkItem rtwq_put_work_item = NULL;
	PFN_RtwqPutWaitingWorkItem rtwq_put_waiting_work_item = NULL;
	bool rtwq_supported = false;
	window_priority priority;
	string window_class;
	string title;
	string executable;
	string session;
	HWND hwnd = NULL;
	DWORD process_id = 0;
	const SourceType sourceType;
	std::atomic<bool> useDeviceTiming = false;
	std::atomic<bool> isDefaultDevice = false;

	bool previouslyFailed = false;
	WinHandle reconnectThread;

	class CallbackStartCapture : public ARtwqAsyncCallback {
	public:
		CallbackStartCapture(WASAPISource *source)
			: ARtwqAsyncCallback(source)
		{
		}

		STDMETHOD(Invoke)
		(IRtwqAsyncResult *) override
		{
			((WASAPISource *)source)->OnStartCapture();
			return S_OK;
		}

	} startCapture;
	ComPtr<IRtwqAsyncResult> startCaptureAsyncResult;

	class CallbackSampleReady : public ARtwqAsyncCallback {
	public:
		CallbackSampleReady(WASAPISource *source)
			: ARtwqAsyncCallback(source)
		{
		}

		STDMETHOD(Invoke)
		(IRtwqAsyncResult *) override
		{
			((WASAPISource *)source)->OnSampleReady();
			return S_OK;
		}
	} sampleReady;
	ComPtr<IRtwqAsyncResult> sampleReadyAsyncResult;

	class CallbackRestart : public ARtwqAsyncCallback {
	public:
		CallbackRestart(WASAPISource *source)
			: ARtwqAsyncCallback(source)
		{
		}

		STDMETHOD(Invoke)
		(IRtwqAsyncResult *) override
		{
			((WASAPISource *)source)->OnRestart();
			return S_OK;
		}
	} restart;
	ComPtr<IRtwqAsyncResult> restartAsyncResult;

	WinHandle captureThread;
	WinHandle idleSignal;
	WinHandle stopSignal;
	WinHandle receiveSignal;
	WinHandle restartSignal;
	WinHandle exitSignal;
	WinHandle initSignal;
	DWORD reconnectDuration = 0;
	WinHandle reconnectSignal;

	speaker_layout speakers;
	audio_format format;
	uint32_t sampleRate;

	uint64_t framesProcessed = 0;

	static DWORD WINAPI ReconnectThread(LPVOID param);
	static DWORD WINAPI CaptureThread(LPVOID param);

	bool ProcessCaptureData();

	void Start();
	void Stop();

	static ComPtr<IMMDevice> _InitDevice(IMMDeviceEnumerator *enumerator,
					    bool isDefaultDevice,
					    SourceType type,
					    string &device_id,
					    string &device_name);
	static ComPtr<IMMDevice> InitDevice(IMMDeviceEnumerator *enumerator,
					    bool isDefaultDevice,
					    SourceType type,
					    string &device_id,
					    string &device_name);
	static ComPtr<IAudioClient> InitClient(
		IMMDevice *device, SourceType type, DWORD process_id,
		PFN_ActivateAudioInterfaceAsync activate_audio_interface_async,
		speaker_layout &speakers, audio_format &format,
		uint32_t &sampleRate);
	static void InitFormat(const WAVEFORMATEX *wfex,
			       enum speaker_layout &speakers,
			       enum audio_format &format, uint32_t &sampleRate);
	static void ClearBuffer(IMMDevice *device);
	static ComPtr<IAudioCaptureClient> InitCapture(IAudioClient *client,
						       HANDLE receiveSignal);
	void Initialize();

	bool TryInitialize();

	struct UpdateParams {
		string device_id;
		bool useDeviceTiming;
		bool isDefaultDevice;
		window_priority priority;
		string window_class;
		string title;
		string executable;
		string session;
	};

	UpdateParams BuildUpdateParams(obs_data_t *settings);
	void UpdateSettings(UpdateParams &&params);

public:
	WASAPISource(obs_data_t *settings, obs_source_t *source_,
		     SourceType type);
	~WASAPISource();

	void Update(obs_data_t *settings);

	void SetDefaultDevice(EDataFlow flow, ERole role, LPCWSTR id);

	void OnStartCapture();
	void OnSampleReady();
	void OnRestart();
};

class WASAPINotify : public IMMNotificationClient {
	long refs = 0; /* auto-incremented to 1 by ComPtr */
	WASAPISource *source;

public:
	WASAPINotify(WASAPISource *source_) : source(source_) {}

	STDMETHODIMP_(ULONG) AddRef()
	{
		return (ULONG)os_atomic_inc_long(&refs);
	}

	STDMETHODIMP_(ULONG) STDMETHODCALLTYPE Release()
	{
		long val = os_atomic_dec_long(&refs);
		if (val == 0)
			delete this;
		return (ULONG)val;
	}

	STDMETHODIMP QueryInterface(REFIID riid, void **ptr)
	{
		if (riid == IID_IUnknown) {
			*ptr = (IUnknown *)this;
		} else if (riid == __uuidof(IMMNotificationClient)) {
			*ptr = (IMMNotificationClient *)this;
		} else {
			*ptr = nullptr;
			return E_NOINTERFACE;
		}

		os_atomic_inc_long(&refs);
		return S_OK;
	}

	STDMETHODIMP OnDefaultDeviceChanged(EDataFlow flow, ERole role,
					    LPCWSTR id)
	{
		source->SetDefaultDevice(flow, role, id);
		return S_OK;
	}

	STDMETHODIMP OnDeviceAdded(LPCWSTR) { return S_OK; }
	STDMETHODIMP OnDeviceRemoved(LPCWSTR) { return S_OK; }
	STDMETHODIMP OnDeviceStateChanged(LPCWSTR, DWORD) { return S_OK; }
	STDMETHODIMP OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY)
	{
		return S_OK;
	}
};

WASAPISource::WASAPISource(obs_data_t *settings, obs_source_t *source_,
			   SourceType type)
	: temp_file(type == SourceType::ProcessOutput
			    ? fopen("wasapi_log.txt", "w")
			    : nullptr),
	  source(source_),
	  sourceType(type),
	  startCapture(this),
	  sampleReady(this),
	  restart(this)
{
	blog(LOG_INFO, "[WASAPISource][%08X] WASAPI Source constructor", this);
	mmdevapi_module = LoadLibrary(L"Mmdevapi");
	if (mmdevapi_module) {
		activate_audio_interface_async =
			(PFN_ActivateAudioInterfaceAsync)GetProcAddress(
				mmdevapi_module, "ActivateAudioInterfaceAsync");
	}

	UpdateSettings(BuildUpdateParams(settings));
	if (device_id.compare("does_not_exist") == 0)
		return;

	idleSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!idleSignal.Valid())
		throw "Could not create idle signal";

	stopSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!stopSignal.Valid())
		throw "Could not create stop signal";

	receiveSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!receiveSignal.Valid())
		throw "Could not create receive signal";

	restartSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!restartSignal.Valid())
		throw "Could not create restart signal";

	exitSignal = CreateEvent(nullptr, true, false, nullptr);
	if (!exitSignal.Valid())
		throw "Could not create exit signal";

	initSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!initSignal.Valid())
		throw "Could not create init signal";

	reconnectSignal = CreateEvent(nullptr, false, false, nullptr);
	if (!reconnectSignal.Valid())
		throw "Could not create reconnect signal";

	reconnectThread = CreateThread(
		nullptr, 0, WASAPISource::ReconnectThread, this, 0, nullptr);
	if (!reconnectThread.Valid())
		throw "Failed to create reconnect thread";

	notify = new WASAPINotify(this);
	if (!notify)
		throw "Could not create WASAPINotify";

	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
				      CLSCTX_ALL,
				      IID_PPV_ARGS(enumerator.Assign()));
	if (FAILED(hr))
		throw HRError("Failed to create enumerator", hr);

	hr = enumerator->RegisterEndpointNotificationCallback(notify);
	if (FAILED(hr))
		throw HRError("Failed to register endpoint callback", hr);

	/* OBS will already load DLL on startup if it exists */
	const HMODULE rtwq_module = GetModuleHandle(L"RTWorkQ.dll");

	// while RTWQ was introduced in Win 8.1, it silently fails
	// to capture Desktop Audio for some reason. Disable for now.
	if (get_win_ver_int() >= _WIN32_WINNT_WIN10)
		rtwq_supported = rtwq_module != NULL;

	if (rtwq_supported) {
		rtwq_unlock_work_queue =
			(PFN_RtwqUnlockWorkQueue)GetProcAddress(
				rtwq_module, "RtwqUnlockWorkQueue");
		rtwq_lock_shared_work_queue =
			(PFN_RtwqLockSharedWorkQueue)GetProcAddress(
				rtwq_module, "RtwqLockSharedWorkQueue");
		rtwq_create_async_result =
			(PFN_RtwqCreateAsyncResult)GetProcAddress(
				rtwq_module, "RtwqCreateAsyncResult");
		rtwq_put_work_item = (PFN_RtwqPutWorkItem)GetProcAddress(
			rtwq_module, "RtwqPutWorkItem");
		rtwq_put_waiting_work_item =
			(PFN_RtwqPutWaitingWorkItem)GetProcAddress(
				rtwq_module, "RtwqPutWaitingWorkItem");

		try {
			hr = rtwq_create_async_result(nullptr, &startCapture,
						      nullptr,
						      &startCaptureAsyncResult);
			if (FAILED(hr)) {
				throw HRError(
					"Could not create startCaptureAsyncResult",
					hr);
			}

			hr = rtwq_create_async_result(nullptr, &sampleReady,
						      nullptr,
						      &sampleReadyAsyncResult);
			if (FAILED(hr)) {
				throw HRError(
					"Could not create sampleReadyAsyncResult",
					hr);
			}

			hr = rtwq_create_async_result(nullptr, &restart,
						      nullptr,
						      &restartAsyncResult);
			if (FAILED(hr)) {
				throw HRError(
					"Could not create restartAsyncResult",
					hr);
			}

			DWORD taskId = 0;
			DWORD id = 0;
			hr = rtwq_lock_shared_work_queue(L"Capture", 0, &taskId,
							 &id);
			if (FAILED(hr)) {
				throw HRError("RtwqLockSharedWorkQueue failed",
					      hr);
			}

			startCapture.SetQueueId(id);
			sampleReady.SetQueueId(id);
			restart.SetQueueId(id);
		} catch (HRError &err) {
			blog(LOG_ERROR, "RTWQ setup failed: %s (0x%08X)",
			     err.str, err.hr);
			rtwq_supported = false;
		}
	}

	if (!rtwq_supported) {
		captureThread = CreateThread(nullptr, 0,
					     WASAPISource::CaptureThread, this,
					     0, nullptr);
		if (!captureThread.Valid()) {
			enumerator->UnregisterEndpointNotificationCallback(
				notify);
			throw "Failed to create capture thread";
		}
	}

	Start();
}

void WASAPISource::Start()
{
	if (rtwq_supported) {
		rtwq_put_work_item(startCapture.GetQueueId(), 0,
				   startCaptureAsyncResult);
	} else {
		SetEvent(initSignal);
	}
}

void WASAPISource::Stop()
{
	if (device_id.compare("does_not_exist") == 0)
		return;

	blog(LOG_INFO, "[WASAPISource::Stop][%08X] Device '%s' Stop called", this,
		device_id.c_str());

	SetEvent(stopSignal);

	blog(LOG_INFO, "[WASAPISource]: Device '%s' Terminated", device_name.c_str());

	if (rtwq_supported)
		SetEvent(receiveSignal);

	WaitForSingleObject(idleSignal, INFINITE);

	SetEvent(exitSignal);

	WaitForSingleObject(reconnectThread, INFINITE);

	if (rtwq_supported)
		rtwq_unlock_work_queue(sampleReady.GetQueueId());
	else
		WaitForSingleObject(captureThread, INFINITE);
}

WASAPISource::~WASAPISource()
{
	blog(LOG_INFO, "[WASAPISource]: 0x%08X Destructor", this);
	if (enumerator.Get() != nullptr && notify.Get() != nullptr)
		enumerator->UnregisterEndpointNotificationCallback(notify);
	Stop();

	if (temp_file)
		fclose(temp_file);
}

WASAPISource::UpdateParams WASAPISource::BuildUpdateParams(obs_data_t *settings)
{
	WASAPISource::UpdateParams params;
	params.device_id = obs_data_get_string(settings, OPT_DEVICE_ID);
	params.useDeviceTiming =
		obs_data_get_bool(settings, OPT_USE_DEVICE_TIMING);
	params.isDefaultDevice =
		_strcmpi(params.device_id.c_str(), "default") == 0;
	params.priority =
		(window_priority)obs_data_get_int(settings, "priority");
	params.window_class.clear();
	params.title.clear();
	params.executable.clear();
	params.session.clear();
	if (sourceType != SourceType::Input) {
		const char *const window =
			obs_data_get_string(settings, OPT_WINDOW);
		if(window[0] == '{') {
			params.session = window;
		} else {
			char *window_class = nullptr;
			char *title = nullptr;
			char *executable = nullptr;
			ms_build_window_strings(window, &window_class, &title,
						&executable);
			if (window_class) {
				params.window_class = window_class;
				bfree(window_class);
			}
			if (title) {
				params.title = title;
				bfree(title);
			}
			if (executable) {
				params.executable = executable;
				bfree(executable);
			}
		}
	}

	return params;
}

void WASAPISource::UpdateSettings(UpdateParams &&params)
{
	device_id = std::move(params.device_id);
	useDeviceTiming = params.useDeviceTiming;
	isDefaultDevice = params.isDefaultDevice;
	priority = params.priority;
	window_class = std::move(params.window_class);
	title = std::move(params.title);
	executable = std::move(params.executable);
	session = std::move(params.session);

	if (sourceType == SourceType::ProcessOutput) {
		blog(LOG_INFO,
		     "[win-wasapi: '%s'] update settings:\n"
		     "\texecutable: %s\n"
		     "\ttitle: %s\n"
		     "\tclass: %s\n"
		     "\tpriority: %d\n"
		     "\tsession: %s\n",
		     obs_source_get_name(source), executable.c_str(),
		     title.c_str(), window_class.c_str(), (int)priority, session.c_str());
	} else {
		blog(LOG_INFO,
		     "[win-wasapi: '%s'] update settings:\n"
		     "\tdevice id: %s\n"
		     "\tuse device timing: %d",
		     obs_source_get_name(source), device_id.c_str(),
		     (int)useDeviceTiming);
	}
}

void WASAPISource::Update(obs_data_t *settings)
{
	UpdateParams params = BuildUpdateParams(settings);

	const bool restart =
		(sourceType == SourceType::ProcessOutput)
			? ((priority != params.priority) ||
			   (window_class != params.window_class) ||
			   (title != params.title) ||
			   (session != params.session) ||
			   (executable != params.executable))
			: (device_id.compare(params.device_id) != 0);

	UpdateSettings(std::move(params));

	if (restart)
		SetEvent(restartSignal);
}

ComPtr<IMMDevice> WASAPISource::_InitDevice(IMMDeviceEnumerator *enumerator,
					   bool isDefaultDevice,
					   SourceType type,
					   string &device_id,
					   string &device_name)
{
	ComPtr<IMMDevice> device;

	if (isDefaultDevice) {
		const bool input = type == SourceType::Input;
		HRESULT res = enumerator->GetDefaultAudioEndpoint(
			input ? eCapture : eRender,
			input ? eCommunications : eConsole, device.Assign());
		if (FAILED(res))
			throw HRError("Failed GetDefaultAudioEndpoint", res);
	} else {
		wchar_t *w_id;
		os_utf8_to_wcs_ptr(device_id.c_str(), device_id.size(), &w_id);
		if (!w_id)
			throw "Failed to widen device id string";

		const HRESULT res =
			enumerator->GetDevice(w_id, device.Assign());

		bfree(w_id);

		if (FAILED(res))
			throw HRError("Failed to enumerate device", res);
	}

	return device;
}

ComPtr<IMMDevice> WASAPISource::InitDevice(IMMDeviceEnumerator *enumerator,
					   bool isDefaultDevice,
					   SourceType type,
					   string &device_id,
					   string &device_name)
{
	ComPtr<IMMDevice> device;
	std::vector<AudioDeviceInfo> devices;
	device = _InitDevice(enumerator, isDefaultDevice, type, device_id, device_name);

	if (device_name.empty())
		device_name = GetDeviceName(device);

	if (device)
		return device;

	if (!device_name.empty()) {
		blog(LOG_INFO,
			"[WASAPISource::InitDevice]: Failed to init device and device name not empty '%s'",
		    device_name.c_str());
		devices.clear();
		GetWASAPIAudioDevices(devices, type == SourceType::Input, device_name);
		if (devices.size()) {
			blog(LOG_INFO,
				"[WASAPISource::InitDevice]: Use divice from GetWASAPIAudioDevices, name '%s'",
			    device_name.c_str());

			device = devices[0].device;
			device_id = devices[0].id;
		}
	}

	return device;
}

#define BUFFER_TIME_100NS (5 * 10000000)

static DWORD GetSpeakerChannelMask(speaker_layout layout)
{
	switch (layout) {
	case SPEAKERS_STEREO:
		return KSAUDIO_SPEAKER_STEREO;
	case SPEAKERS_2POINT1:
		return KSAUDIO_SPEAKER_2POINT1;
	case SPEAKERS_4POINT0:
		return KSAUDIO_SPEAKER_SURROUND;
	case SPEAKERS_4POINT1:
		return OBS_KSAUDIO_SPEAKER_4POINT1;
	case SPEAKERS_5POINT1:
		return KSAUDIO_SPEAKER_5POINT1_SURROUND;
	case SPEAKERS_7POINT1:
		return KSAUDIO_SPEAKER_7POINT1_SURROUND;
	}

	return (DWORD)layout;
}

ComPtr<IAudioClient> WASAPISource::InitClient(
	IMMDevice *device, SourceType type, DWORD process_id,
	PFN_ActivateAudioInterfaceAsync activate_audio_interface_async,
	speaker_layout &speakers, audio_format &format,
	uint32_t &samples_per_sec)
{
	WAVEFORMATEXTENSIBLE wfextensible;
	CoTaskMemPtr<WAVEFORMATEX> wfex;
	const WAVEFORMATEX *pFormat;
	HRESULT res;
	ComPtr<IAudioClient> client;

	if (type == SourceType::ProcessOutput) {
		if (activate_audio_interface_async == NULL)
			throw "ActivateAudioInterfaceAsync is not available";

		struct obs_audio_info oai;
		obs_get_audio_info(&oai);

		const WORD nChannels = (WORD)get_audio_channels(oai.speakers);
		const DWORD nSamplesPerSec = oai.samples_per_sec;
		constexpr WORD wBitsPerSample = 32;
		const WORD nBlockAlign = nChannels * wBitsPerSample / 8;

		WAVEFORMATEX &wf = wfextensible.Format;
		wf.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wf.nChannels = nChannels;
		wf.nSamplesPerSec = nSamplesPerSec;
		wf.nAvgBytesPerSec = nSamplesPerSec * nBlockAlign;
		wf.nBlockAlign = nBlockAlign;
		wf.wBitsPerSample = wBitsPerSample;
		wf.cbSize = sizeof(wfextensible) - sizeof(format);
		wfextensible.Samples.wValidBitsPerSample = wBitsPerSample;
		wfextensible.dwChannelMask =
			GetSpeakerChannelMask(oai.speakers);
		wfextensible.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

		AUDIOCLIENT_ACTIVATION_PARAMS audioclientActivationParams;
		audioclientActivationParams.ActivationType =
			AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
		audioclientActivationParams.ProcessLoopbackParams
			.TargetProcessId = process_id;
		audioclientActivationParams.ProcessLoopbackParams
			.ProcessLoopbackMode =
			PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
		PROPVARIANT activateParams{};
		activateParams.vt = VT_BLOB;
		activateParams.blob.cbSize =
			sizeof(audioclientActivationParams);
		activateParams.blob.pBlobData =
			reinterpret_cast<BYTE *>(&audioclientActivationParams);
		blog(LOG_INFO, "[WASAPISource]: Open audio from a process %d", process_id);

		{
			Microsoft::WRL::ComPtr<
				WASAPIActivateAudioInterfaceCompletionHandler>
				handler = Microsoft::WRL::Make<
					WASAPIActivateAudioInterfaceCompletionHandler>();
			ComPtr<IActivateAudioInterfaceAsyncOperation> asyncOp;
			res = activate_audio_interface_async(
				VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
				__uuidof(IAudioClient), &activateParams,
				handler.Get(), &asyncOp);
			if (FAILED(res))
				throw HRError(
					"Failed to get activate audio client",
					res);

			res = handler->GetActivateResult(client.Assign());
			if (FAILED(res))
				throw HRError("Async activation failed", res);
		}

		pFormat = &wf;
	} else {
		res = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL,
				       nullptr, (void **)client.Assign());
		if (FAILED(res))
			throw HRError("Failed to activate client context", res);

		res = client->GetMixFormat(&wfex);
		if (FAILED(res))
			throw HRError("Failed to get mix format", res);

		pFormat = wfex.Get();
	}

	InitFormat(pFormat, speakers, format, samples_per_sec);

	DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	if (type != SourceType::Input)
		flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
	res = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags,
				 BUFFER_TIME_100NS, 0, pFormat, nullptr);
	if (FAILED(res))
		throw HRError("Failed to initialize audio client", res);

	return client;
}

void WASAPISource::ClearBuffer(IMMDevice *device)
{
	CoTaskMemPtr<WAVEFORMATEX> wfex;
	HRESULT res;
	LPBYTE buffer;
	UINT32 frames;
	ComPtr<IAudioClient> client;

	res = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
			       (void **)client.Assign());
	if (FAILED(res))
		throw HRError("Failed to activate client context", res);

	res = client->GetMixFormat(&wfex);
	if (FAILED(res))
		throw HRError("Failed to get mix format", res);

	res = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, BUFFER_TIME_100NS,
				 0, wfex, nullptr);
	if (FAILED(res))
		throw HRError("Failed to initialize audio client", res);

	/* Silent loopback fix. Prevents audio stream from stopping and */
	/* messing up timestamps and other weird glitches during silence */
	/* by playing a silent sample all over again. */

	res = client->GetBufferSize(&frames);
	if (FAILED(res))
		throw HRError("Failed to get buffer size", res);

	ComPtr<IAudioRenderClient> render;
	res = client->GetService(IID_PPV_ARGS(render.Assign()));
	if (FAILED(res))
		throw HRError("Failed to get render client", res);

	res = render->GetBuffer(frames, &buffer);
	if (FAILED(res))
		throw HRError("Failed to get buffer", res);

	memset(buffer, 0, (size_t)frames * (size_t)wfex->nBlockAlign);

	render->ReleaseBuffer(frames, 0);
}

static speaker_layout ConvertSpeakerLayout(DWORD layout, WORD channels)
{
	switch (layout) {
	case KSAUDIO_SPEAKER_2POINT1:
		return SPEAKERS_2POINT1;
	case KSAUDIO_SPEAKER_SURROUND:
		return SPEAKERS_4POINT0;
	case OBS_KSAUDIO_SPEAKER_4POINT1:
		return SPEAKERS_4POINT1;
	case KSAUDIO_SPEAKER_5POINT1_SURROUND:
		return SPEAKERS_5POINT1;
	case KSAUDIO_SPEAKER_7POINT1_SURROUND:
		return SPEAKERS_7POINT1;
	}

	return (speaker_layout)channels;
}

void WASAPISource::InitFormat(const WAVEFORMATEX *wfex,
			      enum speaker_layout &speakers,
			      enum audio_format &format, uint32_t &sampleRate)
{
	DWORD layout = 0;

	if (wfex->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		WAVEFORMATEXTENSIBLE *ext = (WAVEFORMATEXTENSIBLE *)wfex;
		layout = ext->dwChannelMask;
	}

	/* WASAPI is always float */
	speakers = ConvertSpeakerLayout(layout, wfex->nChannels);
	format = AUDIO_FORMAT_FLOAT;
	sampleRate = wfex->nSamplesPerSec;
}

ComPtr<IAudioCaptureClient> WASAPISource::InitCapture(IAudioClient *client,
						      HANDLE receiveSignal)
{
	ComPtr<IAudioCaptureClient> capture;
	HRESULT res = client->GetService(IID_PPV_ARGS(capture.Assign()));
	if (FAILED(res))
		throw HRError("Failed to create capture context", res);

	res = client->SetEventHandle(receiveSignal);
	if (FAILED(res))
		throw HRError("Failed to set event handle", res);

	res = client->Start();
	if (FAILED(res))
		throw HRError("Failed to start capture client", res);

	return capture;
}

void WASAPISource::Initialize()
{
	ComPtr<IMMDevice> device;
	if (sourceType == SourceType::ProcessOutput) {
		device_name = "[VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK]";

		if(session.size()== 0) {
			hwnd = ms_find_window(INCLUDE_MINIMIZED, priority,
					window_class.c_str(), title.c_str(),
					executable.c_str());
			if (!hwnd)
				throw HRError("Failed to find window", 0);

			DWORD dwProcessId = 0;
			if (!GetWindowThreadProcessId(hwnd, &dwProcessId)) {
				hwnd = NULL;
				throw HRError("Failed to get process id of window", 0);
			}
			process_id = dwProcessId;
		} else {
			process_id = AppDevicesCache::getInstance()->getPID(session);
			if(process_id == 0)
				throw HRError("Failed to get process id of session", 0);
		}
	} else {
		device = InitDevice(enumerator, isDefaultDevice, sourceType,
				    device_id, device_name);

		device_name = GetDeviceName(device);
	}

	ResetEvent(receiveSignal);

	ComPtr<IAudioClient> temp_client = InitClient(
		device, sourceType, process_id, activate_audio_interface_async,
		speakers, format, sampleRate);
	if (sourceType == SourceType::DeviceOutput)
		ClearBuffer(device);
	ComPtr<IAudioCaptureClient> temp_capture =
		InitCapture(temp_client, receiveSignal);

	client = std::move(temp_client);
	capture = std::move(temp_capture);

	if (rtwq_supported) {
		HRESULT hr = rtwq_put_waiting_work_item(
			receiveSignal, 0, sampleReadyAsyncResult, nullptr);
		if (FAILED(hr)) {
			capture.Clear();
			client.Clear();
			throw HRError("RtwqPutWaitingWorkItem sampleReadyAsyncResult failed", hr);
		}

		hr = rtwq_put_waiting_work_item(restartSignal, 0,
						restartAsyncResult, nullptr);
		if (FAILED(hr)) {
			capture.Clear();
			client.Clear();
			throw HRError("RtwqPutWaitingWorkItem restartAsyncResult failed", hr);
		}
	}

	blog(LOG_INFO, "[WASAPISource]: Device '%s' [%" PRIu32 " Hz] initialized",
	     device_name.c_str(), sampleRate);
}

bool WASAPISource::TryInitialize()
{
	bool success = false;
	try {
		Initialize();
		success = true;
	} catch (HRError &error) {
		if (true) { // !previouslyFailed 
			blog(LOG_WARNING,
			     "[WASAPISource::TryInitialize]:[%s] %s: %lX",
			     device_name.empty() ? device_id.c_str()
						 : device_name.c_str(),
			     error.str, error.hr);
		}
	} catch (...) {
		blog(LOG_DEBUG, "[WASAPISource::TryInitialize] Catch exception");
	}

	previouslyFailed = !success;
	return success;
}

DWORD WINAPI WASAPISource::ReconnectThread(LPVOID param)
{
	os_set_thread_name("win-wasapi: reconnect thread");

	WASAPISource *source = (WASAPISource *)param;

	const HANDLE sigs[] = {
		source->exitSignal,
		source->reconnectSignal,
	};

	bool exit = false;
	while (!exit) {
		const DWORD ret = WaitForMultipleObjects(_countof(sigs), sigs,
							 false, INFINITE);
		switch (ret) {
		case WAIT_OBJECT_0:
			exit = true;
			break;
		default:
			assert(ret == (WAIT_OBJECT_0 + 1));
			if (source->reconnectDuration > 0) {
				WaitForSingleObject(source->stopSignal,
						    source->reconnectDuration);
			}
			source->Start();
		}
	}

	return 0;
}

bool WASAPISource::ProcessCaptureData()
{
	HRESULT res;
	LPBYTE buffer;
	UINT32 frames;
	DWORD flags;
	UINT64 pos, ts;
	UINT captureSize = 0;

	while (true) {
		if ((sourceType == SourceType::ProcessOutput) &&
		    !IsWindow(hwnd)) {
			blog(LOG_WARNING,
			     "[WASAPISource::ProcessCaptureData] window disappeared");
			return false;
		}

		res = capture->GetNextPacketSize(&captureSize);
		if (FAILED(res)) {
			if (res != AUDCLNT_E_DEVICE_INVALIDATED)
				blog(LOG_WARNING,
				     "[WASAPISource::ProcessCaptureData]"
				     " capture->GetNextPacketSize"
				     " failed: %lX",
				     res);
			return false;
		}

		if (!captureSize)
			break;

		res = capture->GetBuffer(&buffer, &frames, &flags, &pos, &ts);
		if (FAILED(res)) {
			if (res != AUDCLNT_E_DEVICE_INVALIDATED)
				blog(LOG_WARNING,
				     "[WASAPISource::ProcessCaptureData]"
				     " capture->GetBuffer"
				     " failed: %lX",
				     res);
			return false;
		}

		obs_source_audio data = {};
		data.data[0] = (const uint8_t *)buffer;
		data.frames = (uint32_t)frames;
		data.speakers = speakers;
		data.samples_per_sec = sampleRate;
		data.format = format;
		if (sourceType == SourceType::ProcessOutput) {
			data.timestamp = util_mul_div64(framesProcessed,
							UINT64_C(1000000000),
							sampleRate);
			framesProcessed += frames;

			if (temp_file) {
				LARGE_INTEGER count;
				QueryPerformanceCounter(&count);
				fprintf(temp_file,
					"%lu\t%" PRIu64 "\t%" PRIu64
					"\t%" PRIu32 "\t%lld\n",
					flags, pos, ts, frames, count.QuadPart);
			}
		} else {
			data.timestamp = useDeviceTiming ? ts * 100
							 : os_gettime_ns();

			if (!useDeviceTiming)
				data.timestamp -= util_mul_div64(
					frames, UINT64_C(1000000000),
					sampleRate);
		}

		obs_source_output_audio(source, &data);

		capture->ReleaseBuffer(frames);
	}

	return true;
}

#define RECONNECT_INTERVAL 3000

DWORD WINAPI WASAPISource::CaptureThread(LPVOID param)
{
	os_set_thread_name("win-wasapi: capture thread");

	const HRESULT hr = CoInitializeEx(0, COINIT_MULTITHREADED);
	const bool com_initialized = SUCCEEDED(hr);
	if (!com_initialized) {
		blog(LOG_ERROR,
		     "[WASAPISource::CaptureThread]"
		     " CoInitializeEx failed: 0x%08X",
		     hr);
	}

	DWORD unused = 0;
	const HANDLE handle = AvSetMmThreadCharacteristics(L"Audio", &unused);

	WASAPISource *source = (WASAPISource *)param;

	const HANDLE inactive_sigs[] = {
		source->exitSignal,
		source->stopSignal,
		source->initSignal,
	};

	const HANDLE active_sigs[] = {
		source->exitSignal,
		source->stopSignal,
		source->receiveSignal,
		source->restartSignal,
	};

	DWORD sig_count = _countof(inactive_sigs);
	const HANDLE *sigs = inactive_sigs;

	bool exit = false;
	while (!exit) {
		bool idle = false;
		bool stop = false;
		bool reconnect = false;
		do {
			/* Windows 7 does not seem to wake up for LOOPBACK */
			const DWORD dwMilliseconds =
				((sigs == active_sigs) &&
				 (source->sourceType != SourceType::Input))
					? 10
					: INFINITE;

			const DWORD ret = WaitForMultipleObjects(
				sig_count, sigs, false, dwMilliseconds);
			switch (ret) {
			case WAIT_OBJECT_0: {
				exit = true;
				stop = true;
				idle = true;
				break;
			}

			case WAIT_OBJECT_0 + 1:
				stop = true;
				idle = true;
				break;

			case WAIT_OBJECT_0 + 2:
			case WAIT_TIMEOUT:
				if (sigs == inactive_sigs) {
					assert(ret != WAIT_TIMEOUT);

					if (source->TryInitialize()) {
						sig_count =
							_countof(active_sigs);
						sigs = active_sigs;
					} else {
						blog(LOG_INFO,
						     "[WASAPISource::CaptureThread] Device '%s' failed to start",
						     source->device_id.c_str());
						stop = true;
						reconnect = true;
						source->reconnectDuration =
							RECONNECT_INTERVAL;
					}
				} else {
					stop = !source->ProcessCaptureData();
					if (stop) {
						blog(LOG_INFO,
						     "[WASAPISource::CaptureThread] Device '%s' invalidated.  Retrying",
						     source->device_name
							     .c_str());
						stop = true;
						reconnect = true;
						source->reconnectDuration =
							RECONNECT_INTERVAL;
					}
				}
				break;

			default:
				assert(sigs == active_sigs);
				assert(ret == WAIT_OBJECT_0 + 3);
				stop = true;
				reconnect = true;
				source->reconnectDuration = 0;
				ResetEvent(source->restartSignal);
			}
		} while (!stop);

		sig_count = _countof(inactive_sigs);
		sigs = inactive_sigs;

		if (source->client) {
			source->client->Stop();

			source->capture.Clear();
			source->client.Clear();
		}

		if (idle) {
			SetEvent(source->idleSignal);
		} else if (reconnect) {
			blog(LOG_INFO, "[WASAPISource::CaptureThread] Device '%s' invalidated.  Retrying",
			     source->device_name.c_str());
			SetEvent(source->reconnectSignal);
		}
	}

	if (handle)
		AvRevertMmThreadCharacteristics(handle);

	if (com_initialized)
		CoUninitialize();

	return 0;
}

void WASAPISource::SetDefaultDevice(EDataFlow flow, ERole role, LPCWSTR id)
{
	if (!isDefaultDevice)
		return;

	const bool input = sourceType == SourceType::Input;
	const EDataFlow expectedFlow = input ? eCapture : eRender;
	const ERole expectedRole = input ? eCommunications : eConsole;
	if (flow != expectedFlow || role != expectedRole)
		return;

	if (id) {
		if (default_id.compare(id) == 0)
			return;
		default_id = id;
	} else {
		if (default_id.empty())
			return;
		default_id.clear();
	}

	blog(LOG_INFO, "[WASAPISource::SetDefaultDevice][%08X] Default device changed, name was '%s'",
	     this, device_name.empty() ? device_id.c_str() : device_name.c_str());

	SetEvent(restartSignal);
}

void WASAPISource::OnStartCapture()
{
	blog(LOG_INFO, "[WASAPISource::OnStartCapture] Device '%s' function called",
		device_id.c_str());
	const DWORD ret = WaitForSingleObject(stopSignal, 0);
	switch (ret) {
	case WAIT_OBJECT_0:
		SetEvent(idleSignal);
		break;

	default:
		assert(ret == WAIT_TIMEOUT);

		if (!TryInitialize()) {
			blog(LOG_INFO, "[WASAPISource::OnStartCapture] Device '%s' failed to start",
			     device_id.c_str());
			reconnectDuration = RECONNECT_INTERVAL;
			SetEvent(reconnectSignal);
		}
	}
}

void WASAPISource::OnSampleReady()
{
	bool stop = false;
	bool reconnect = false;

	if (!ProcessCaptureData()) {
		stop = true;
		reconnect = true;
		reconnectDuration = RECONNECT_INTERVAL;
	}

	if (WaitForSingleObject(restartSignal, 0) == WAIT_OBJECT_0) {
		stop = true;
		reconnect = true;
		reconnectDuration = 0;

		ResetEvent(restartSignal);
		rtwq_put_waiting_work_item(restartSignal, 0, restartAsyncResult,
					   nullptr);
	}

	if (WaitForSingleObject(stopSignal, 0) == WAIT_OBJECT_0) {
		stop = true;
		reconnect = false;
	}

	if (!stop) {
		if (FAILED(rtwq_put_waiting_work_item(receiveSignal, 0,
						      sampleReadyAsyncResult,
						      nullptr))) {
			blog(LOG_ERROR,
			     "[WASAPISource] Could not requeue sample receive work");
			stop = true;
			reconnect = true;
			reconnectDuration = RECONNECT_INTERVAL;
		}
	}

	if (stop) {
		client->Stop();

		capture.Clear();
		client.Clear();

		if (reconnect) {
			blog(LOG_INFO, "[WASAPISource] Device '%s' invalidated.  Retrying",
			     device_name.c_str());
			SetEvent(reconnectSignal);
		} else {
			SetEvent(idleSignal);
		}
	}
}

void WASAPISource::OnRestart()
{
	SetEvent(receiveSignal);
}

/* ------------------------------------------------------------------------- */

static const char *GetWASAPIInputName(void *)
{
	return obs_module_text("AudioInput");
}

static const char *GetWASAPIDeviceOutputName(void *)
{
	return obs_module_text("AudioOutput");
}

static const char *GetWASAPIProcessOutputName(void *)
{
	return obs_module_text("ApplicationAudioCapture");
}

static void GetWASAPIDefaultsInput(obs_data_t *settings)
{
	obs_data_set_default_string(settings, OPT_DEVICE_ID, "default");
	obs_data_set_default_bool(settings, OPT_USE_DEVICE_TIMING, false);
}

static void GetWASAPIDefaultsDeviceOutput(obs_data_t *settings)
{
	obs_data_set_default_string(settings, OPT_DEVICE_ID, "default");
	obs_data_set_default_bool(settings, OPT_USE_DEVICE_TIMING, true);
}

static void GetWASAPIDefaultsProcessOutput(obs_data_t *settings)
{
	obs_data_set_default_string(settings, OPT_DEVICE_ID, "");
	obs_data_set_default_bool(settings, OPT_USE_DEVICE_TIMING, true);
	obs_data_set_default_int(settings, OPT_PRIORITY, WINDOW_PRIORITY_EXE);
}

static void *CreateWASAPISource(obs_data_t *settings, obs_source_t *source,
				SourceType type)
{
	try {
		return new WASAPISource(settings, source, type);
	} catch (const char *error) {
		blog(LOG_ERROR, "[WASAPISource][CreateWASAPISource] Catch %s", error);
	}
	AppDevicesCache::addRef();
	return nullptr;
}

typedef void(WINAPI *RtlGetVersion)(OSVERSIONINFOEXW *);
bool isMediaCrashPatchNeeded()
{
	static bool first_attempt = true;
	if (!first_attempt) {
		return false;
	}
	first_attempt = false;

	OSVERSIONINFOEXW osw = {0};
	RtlGetVersion getVersion;
	HMODULE ntdllmodule = LoadLibrary(TEXT("ntdll.dll"));
	if (ntdllmodule) {
		getVersion = (RtlGetVersion)GetProcAddress(ntdllmodule, "RtlGetVersion");
		if (getVersion == 0) {
			FreeLibrary(ntdllmodule);
			return false;
		}
		osw.dwOSVersionInfoSize = sizeof(osw);
		getVersion(&osw);
		blog(LOG_DEBUG, "[MEDIADLLPATCH] windows version %d %d %d %d ",
		     osw.dwBuildNumber, osw.dwMinorVersion, osw.dwMajorVersion, osw.dwPlatformId);
		if (osw.dwBuildNumber == 22000 && osw.dwMinorVersion == 0 && osw.dwMajorVersion == 10) {
			return true;
		}
	}
	return false;
}

void patchMediaCrash()
{
	if (!isMediaCrashPatchNeeded()) {
		return;
	}

	static const uint8_t patterndata[] = {0x83, 0xF9, 0x08, 0xB8, 0x04, 0x00, 0x00,
				       0x00, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00,
				       0X00, 0X00, 0X00, 0X00, 0x83, 0X00, 0X00,
				       0X00, 0x44, 0X00, 0X00, 0X00, 0x0F, 0X00,
				       0X00, 0X00, 0X00, 0X00, 0xC7};
	static const uint8_t patternsten[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				       0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				       0X01, 0X01, 0X01, 0X01, 0x00, 0X01, 0X01,
				       0X01, 0x00, 0X01, 0X01, 0X01, 0x00, 0X01,
				       0X01, 0X01, 0X01, 0X01, 0x00};

	static const uint8_t patchdata[] = {0x83, 0xF9, 0x08, 0xB8, 0x04, 0x00, 0x00,
				     0x00, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00,
				     0X00, 0X00, 0X00, 0X00, 0x83, 0X00, 0X00,
				     0X00, 0x44, 0X00, 0X00, 0X00, 0x90, 0x90,
				     0x90, 0x90, 0x90, 0x90, 0xC7};
	static const uint8_t patchsten[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
				     0X01, 0X01, 0X01, 0X01, 0x00, 0X01, 0X01,
				     0X01, 0x00, 0X01, 0X01, 0X01, 0x00, 0x00,
				     0x00, 0x00, 0x00, 0x00, 0x00};

	MODULEINFO module_info = {0};
	HMODULE media_module = LoadLibrary(L"Windows.Media.MediaControl.dll");
	if (media_module) {
		BOOL ret = GetModuleInformation(GetCurrentProcess(), media_module, &module_info, sizeof(module_info));
		if (!ret) {
			blog(LOG_DEBUG, "[MEDIADLLPATCH] failed to get module info %d", GetLastError());
			return;
		}
	} else {
		blog(LOG_DEBUG, "[MEDIADLLPATCH] failed to get module %d", GetLastError());
		return;
	}
	blog(LOG_DEBUG, "[MEDIADLLPATCH] MediaControl dll module info: start %d size %d", module_info.lpBaseOfDll, module_info.SizeOfImage);

	uint8_t *found_memory = nullptr;
	for (size_t offset = 0; offset < module_info.SizeOfImage - sizeof(patterndata); offset++) {

		for (size_t pattern_offset = 0; pattern_offset < sizeof(patterndata); pattern_offset++) {

			uint8_t memory_byte = *((uint8_t *)module_info.lpBaseOfDll + offset + pattern_offset);
			uint8_t pattern_byte = *(patterndata + pattern_offset);
			uint8_t pattern_stencil_byte = *(patternsten + pattern_offset);
			if (pattern_stencil_byte || pattern_byte == memory_byte) {
				found_memory = (uint8_t *)module_info.lpBaseOfDll + offset;
				continue;
			} else {
				found_memory = nullptr;
				break;
			}
		}
		if (found_memory)
			break;
	}

	if (found_memory != nullptr) {
		blog(LOG_DEBUG, "[MEDIADLLPATCH] memory pattern start %d", found_memory);
		DWORD prev_flags = 0;
		if (VirtualProtect(found_memory, sizeof(patchdata), PAGE_EXECUTE_READWRITE, &prev_flags)) {

			for (size_t patch_offset = 0; patch_offset < sizeof(patchdata); patch_offset++) {
				uint8_t *memory_offset = found_memory + patch_offset;
				if (patchsten[patch_offset] == 0x00) {
					*memory_offset = patchdata[patch_offset];
				}
			}
			VirtualProtect(found_memory, sizeof(patchdata), prev_flags, nullptr);
		} else {
			blog(LOG_DEBUG, "[MEDIADLLPATCH] failed to unlock memory");
		}
	} else {
		blog(LOG_DEBUG, "[MEDIADLLPATCH] failed to found memory pattern");
	}
}

static void *CreateWASAPIInput(obs_data_t *settings, obs_source_t *source)
{
	return CreateWASAPISource(settings, source, SourceType::Input);
}

static void *CreateWASAPIDeviceOutput(obs_data_t *settings,
				      obs_source_t *source)
{
	return CreateWASAPISource(settings, source, SourceType::DeviceOutput);
}

static void *CreateWASAPIProcessOutput(obs_data_t *settings,
				       obs_source_t *source)
{
	return CreateWASAPISource(settings, source, SourceType::ProcessOutput);
}

static void DestroyWASAPISource(void *obj)
{
	AppDevicesCache::releaseRef();
	delete static_cast<WASAPISource *>(obj);
}

static void UpdateWASAPISource(void *obj, obs_data_t *settings)
{
	static_cast<WASAPISource *>(obj)->Update(settings);
}

static bool UpdateWASAPIMethod(obs_properties_t *props, obs_property_t *,
			       obs_data_t *settings)
{
	WASAPISource *source = (WASAPISource *)obs_properties_get_param(props);
	if (!source)
		return false;

	source->Update(settings);

	return true;
}

static obs_properties_t *GetWASAPIPropertiesInput(void *)
{
	obs_properties_t *props = obs_properties_create();
	vector<AudioDeviceInfo> devices;

	obs_property_t *device_prop = obs_properties_add_list(
		props, OPT_DEVICE_ID, obs_module_text("Device"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	GetWASAPIAudioDevices(devices, true);

	if (devices.size())
		obs_property_list_add_string(
			device_prop, obs_module_text("Default"), "default");

	for (size_t i = 0; i < devices.size(); i++) {
		AudioDeviceInfo &device_i = devices[i];
		obs_property_list_add_string(device_prop, device_i.name.c_str(),
					     device_i.id.c_str());
	}

	obs_properties_add_bool(props, OPT_USE_DEVICE_TIMING,
				obs_module_text("UseDeviceTiming"));

	return props;
}

static obs_properties_t *GetWASAPIPropertiesDeviceOutput(void *)
{
	obs_properties_t *props = obs_properties_create();
	vector<AudioDeviceInfo> devices;

	obs_property_t *device_prop = obs_properties_add_list(
		props, OPT_DEVICE_ID, obs_module_text("Device"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	GetWASAPIAudioDevices(devices, false);

	if (devices.size())
		obs_property_list_add_string(
			device_prop, obs_module_text("Default"), "default");

	for (size_t i = 0; i < devices.size(); i++) {
		AudioDeviceInfo &device = devices[i];
		obs_property_list_add_string(device_prop, device.name.c_str(),
					     device.id.c_str());
	}

	obs_properties_add_bool(props, OPT_USE_DEVICE_TIMING,
				obs_module_text("UseDeviceTiming"));

	return props;
}

static obs_properties_t *GetWASAPIPropertiesProcessOutput(void *)
{
	obs_properties_t *props = obs_properties_create();

	obs_property_t *const window_prop = obs_properties_add_list(
		props, OPT_WINDOW, obs_module_text("Window"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_apps_list(window_prop, INCLUDE_MINIMIZED);

	obs_property_t *const priority_prop = obs_properties_add_list(
		props, OPT_PRIORITY, obs_module_text("Priority"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(priority_prop,
				  obs_module_text("Priority.Title"),
				  WINDOW_PRIORITY_TITLE);
	obs_property_list_add_int(priority_prop,
				  obs_module_text("Priority.Class"),
				  WINDOW_PRIORITY_CLASS);
	obs_property_list_add_int(priority_prop,
				  obs_module_text("Priority.Exe"),
				  WINDOW_PRIORITY_EXE);

	return props;
}

void RegisterWASAPIInput()
{
	obs_source_info info = {};
	info.id = "wasapi_input_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.get_name = GetWASAPIInputName;
	info.create = CreateWASAPIInput;
	info.destroy = DestroyWASAPISource;
	info.update = UpdateWASAPISource;
	info.get_defaults = GetWASAPIDefaultsInput;
	info.get_properties = GetWASAPIPropertiesInput;
	info.icon_type = OBS_ICON_TYPE_AUDIO_INPUT;
	obs_register_source(&info);
}

void RegisterWASAPIDeviceOutput()
{
	obs_source_info info = {};
	info.id = "wasapi_output_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.get_name = GetWASAPIDeviceOutputName;
	info.create = CreateWASAPIDeviceOutput;
	info.destroy = DestroyWASAPISource;
	info.update = UpdateWASAPISource;
	info.get_defaults = GetWASAPIDefaultsDeviceOutput;
	info.get_properties = GetWASAPIPropertiesDeviceOutput;
	info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;
	obs_register_source(&info);
}

void RegisterWASAPIProcessOutput()
{
	obs_source_info info = {};
	info.id = "wasapi_app_capture";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE |
			    OBS_SOURCE_DO_NOT_SELF_MONITOR;
	info.get_name = GetWASAPIProcessOutputName;
	info.create = CreateWASAPIProcessOutput;
	info.destroy = DestroyWASAPISource;
	info.update = UpdateWASAPISource;
	info.get_defaults = GetWASAPIDefaultsProcessOutput;
	info.get_properties = GetWASAPIPropertiesProcessOutput;
	info.icon_type = OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT;
	obs_register_source(&info);
}
