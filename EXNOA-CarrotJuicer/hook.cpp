#include <filesystem>
#include <iostream>
#include <locale>
#include <string>
#include <thread>
#include <Windows.h>
#include <MinHook.h>
#include "config.hpp"
#include "edb.hpp"
#include "responses.hpp"
#include "notifier.hpp"
#include "requests.hpp"
#include "il2cpp_symbols.hpp"

using namespace std::literals;

namespace
{
	void dump_bytes(void* pos)
	{
		printf("Hex dump of %p\n", pos);

		char* memory = reinterpret_cast<char*>(pos);

		for (int i = 0; i < 0x20; i++)
		{
			if (i > 0 && i % 16 == 0)
				printf("\n");

			char byte = *(memory++);

			printf("%02hhX ", byte);
		}

		printf("\n\n");
	}

	void create_debug_console()
	{
		AllocConsole();

		FILE* _;
		// open stdout stream
		freopen_s(&_, "CONOUT$", "w", stdout);
		freopen_s(&_, "CONOUT$", "w", stderr);
		freopen_s(&_, "CONIN$", "r", stdin);

		SetConsoleTitle(L"Umapyoi");

		// set this to avoid turn japanese texts into question mark
		SetConsoleOutputCP(CP_UTF8);
		std::locale::global(std::locale(""));

		const HANDLE handle = CreateFile(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
		                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD mode;
		if (!GetConsoleMode(handle, &mode))
		{
			std::cout << "GetConsoleMode " << GetLastError() << "\n";
		}
		mode |= ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
		if (!SetConsoleMode(handle, mode))
		{
			std::cout << "SetConsoleMode " << GetLastError() << "\n";
		}
	}

	std::string current_time()
	{
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::system_clock::now().time_since_epoch());
		return std::to_string(ms.count());
	}

	void write_file(const std::string& file_name, const char* buffer, const int len)
	{
		FILE* fp;
		fopen_s(&fp, file_name.c_str(), "wb");
		if (fp != nullptr)
		{
			fwrite(buffer, 1, len, fp);
			fclose(fp);
		}
	}

	void* set_fps_orig = nullptr;
	int fps_num = config::get().fps_hack;
	void set_fps_hook(int value)
	{
		return reinterpret_cast<decltype(set_fps_hook)*>(set_fps_orig)(fps_num);
	}


	void* LZ4_decompress_safe_ext_orig = nullptr;

	int LZ4_decompress_safe_ext_hook(
		char* src,
		char* dst,
		int compressedSize,
		int dstCapacity)
	{
		const int ret = reinterpret_cast<decltype(LZ4_decompress_safe_ext_hook)*>(LZ4_decompress_safe_ext_orig)(
			src, dst, compressedSize, dstCapacity);


		const std::string data(dst, ret);

		auto notifier_thread = std::thread([&]
		{
			notifier::notify_response(data);
		});

		responses::print_response_additional_info(data);

		notifier_thread.join();

		return ret;
	}


//#pragma region HOOK_ADDRESSES
//	auto set_fps_addr = il2cpp_symbols::get_method_pointer(
//		"UnityEngine.CoreModule.dll", "UnityEngine",
//		"Application", "set_targetFrameRate", 1
//	);
//#pragma endregion
	void bootstrap_carrot_juicer()
	{
		std::filesystem::create_directory("CarrotJuicer");
		const auto il2cpp_module = GetModuleHandle(L"GameAssembly.dll");

		// load il2cpp exported functions
		il2cpp_symbols::init(il2cpp_module);

		const auto libnative_module = GetModuleHandle(L"libnative.dll");
		printf("libnative.dll at %p\n", libnative_module);
		if (libnative_module == nullptr)
		{
			return;
		}

		const auto LZ4_decompress_safe_ext_ptr = GetProcAddress(libnative_module, "LZ4_decompress_safe_ext");
		printf("LZ4_decompress_safe_ext at %p\n", LZ4_decompress_safe_ext_ptr);
		if (LZ4_decompress_safe_ext_ptr == nullptr)
		{
			return;
		}
		MH_CreateHook(LZ4_decompress_safe_ext_ptr, LZ4_decompress_safe_ext_hook, &LZ4_decompress_safe_ext_orig);
		MH_EnableHook(LZ4_decompress_safe_ext_ptr);

		auto set_fps_addr = il2cpp_symbols::get_method_pointer(
					"UnityEngine.CoreModule.dll", "UnityEngine",
					"Application", "set_targetFrameRate", 1
				);
		const auto set_fps_ptr = reinterpret_cast<void*>(set_fps_addr);
		printf("set_fps at %p\n", set_fps_addr);
		if (set_fps_ptr == nullptr)
		{
			return;
		}
		MH_CreateHook(set_fps_ptr, set_fps_hook, &set_fps_orig);
		MH_EnableHook(set_fps_ptr);
	}




	void* load_library_w_orig = nullptr;

	HMODULE __stdcall load_library_w_hook(const wchar_t* path)
	{
		printf("Saw %ls\n", path);

		// GameAssembly.dll code must be loaded and decrypted while loading criware library
		if (path == L"cri_ware_unity.dll"s)
		{
			bootstrap_carrot_juicer();

			MH_DisableHook(LoadLibraryW);
			MH_RemoveHook(LoadLibraryW);

			return LoadLibraryW(path);
		}

		return reinterpret_cast<decltype(LoadLibraryW)*>(load_library_w_orig)(path);
	}
}

void attach()
{
	create_debug_console();

	if (MH_Initialize() != MH_OK)
	{
		printf("Failed to initialize MinHook.\n");
		return;
	}
	printf("MinHook initialized.\n");

	MH_CreateHook(LoadLibraryW, load_library_w_hook, &load_library_w_orig);
	MH_EnableHook(LoadLibraryW);

	config::load();

	std::thread(edb::init).detach();
	std::thread(notifier::init).detach();
}

void detach()
{
	MH_DisableHook(MH_ALL_HOOKS);
	MH_Uninitialize();
}
