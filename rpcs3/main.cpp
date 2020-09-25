﻿// Qt5.10+ frontend implementation for rpcs3. Known to work on Windows, Linux, Mac
// by Sacha Refshauge, Megamouse and flash-fire

#include <fstream>//RTC_Hijack: include these for the gameinfo writing
#include <string> //RTC_Hijack: include these for the gameinfo writing
#include <iostream>

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QTimer>
#include <QObject>
#include <QStyleFactory>

#include "rpcs3qt/gui_application.h"
#include "rpcs3qt/fatal_error_dialog.h"

#include "headless_application.h"
#include "Utilities/sema.h"
#ifdef _WIN32
#include <windows.h>
#include "Utilities/dynamic_library.h"
DYNAMIC_IMPORT("ntdll.dll", NtQueryTimerResolution, NTSTATUS(PULONG MinimumResolution, PULONG MaximumResolution, PULONG CurrentResolution));
DYNAMIC_IMPORT("ntdll.dll", NtSetTimerResolution, NTSTATUS(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution));
#else
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <limits.h>
#endif

#ifdef __linux__
#include <sys/time.h>
#include <sys/resource.h>
#endif

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

#include "Utilities/sysinfo.h"
#include "Utilities/Config.h"
#include "rpcs3_version.h"
#include "Emu/System.h"
#include <thread>
#include <charconv>
#include <Crypto\unself.cpp> //RTC_Hijack: include unself.cpp so decryption via command line is possible
#include <Emu\GameInfo.h> //RTC_Hijack: Include game info header
#include "Loader/PSF.h" //RTC_Hijack: include PSF

inline std::string sstr(const QString& _in) { return _in.toStdString(); }

static semaphore<> s_qt_init;

static atomic_t<bool> s_headless = false;
static atomic_t<char*> s_argv0;

#ifndef _WIN32
extern char **environ;
#endif

LOG_CHANNEL(sys_log, "SYS");

[[noreturn]] extern void report_fatal_error(const std::string& text)
{
	if (s_headless)
	{
		fprintf(stderr, "RPCS3: %s\n", text.c_str());
		std::abort();
	}

	const bool local = s_qt_init.try_lock();

	// Possibly created and assigned here
	QScopedPointer<QCoreApplication> app;

	if (local)
	{
		static int argc = 1;
		static char* argv[] = {+s_argv0};
		app.reset(new QApplication{argc, argv});
	}
	else
	{
		fprintf(stderr, "RPCS3: %s\n", text.c_str());
	}

	auto show_report = [](const std::string& text)
	{
		fatal_error_dialog dlg(text);
		dlg.exec();
	};

#ifdef __APPLE__
	// Cocoa access is not allowed outside of the main thread
	if (!pthread_main_np())
	{
		dispatch_sync(dispatch_get_main_queue(), ^ { show_report(text); });
	}
	else
#endif
	{
		// If Qt is already initialized, spawn a new RPCS3 process with an --error argument
		if (local)
		{
			// Since we only show an error, we can hope for a graceful exit
			show_report(text);
			app.reset();
			std::exit(0);
		}
		else
		{
#ifdef _WIN32
			wchar_t buffer[32767];
			GetModuleFileNameW(nullptr, buffer, sizeof(buffer) / 2);
			std::wstring arg(text.cbegin(), text.cend()); // ignore unicode for now
			_wspawnl(_P_WAIT, buffer, buffer, L"--error", arg.c_str(), nullptr);
#else
			pid_t pid;
			std::vector<char> data(text.data(), text.data() + text.size() + 1);
			std::string run_arg = +s_argv0;
			std::string err_arg = "--error";

			if (run_arg.find_first_of('/') == umax)
			{
				// AppImage has "rpcs3" in argv[0], can't just execute it
#ifdef __linux__
				char buffer[PATH_MAX]{};
				if (::readlink("/proc/self/exe", buffer, sizeof(buffer) - 1) > 0)
				{
					printf("Found exec link: %s\n", buffer);
					run_arg = buffer;
				}
#endif
			}

			char* argv[] = {run_arg.data(), err_arg.data(), data.data(), nullptr};
			int ret = posix_spawn(&pid, run_arg.c_str(), nullptr, nullptr, argv, environ);

			if (ret == 0)
			{
				int status;
				waitpid(pid, &status, 0);
			}
			else
			{
				fprintf(stderr, "posix_spawn() failed: %d\n", ret);
			}
#endif
			std::abort();
		}
	}

	std::abort();
}

struct pause_on_fatal final : logs::listener
{
	~pause_on_fatal() override = default;

	void log(u64 /*stamp*/, const logs::message& msg, const std::string& /*prefix*/, const std::string& /*text*/) override
	{
		if (msg.sev == logs::level::fatal)
		{
			// Pause emulation if fatal error encountered
			Emu.Pause();
		}
	}
};

const char* arg_headless   = "headless";
const char* arg_no_gui     = "no-gui";
const char* arg_high_dpi   = "hidpi";
const char* arg_rounding   = "dpi-rounding";
const char* arg_styles     = "styles";
const char* arg_style      = "style";
const char* arg_stylesheet = "stylesheet";
const char* arg_config     = "config";
const char* arg_error      = "error";
const char* arg_updating   = "updating";
const char* arg_decrypt    = "decrypt"; //RTC_Hijack: add decrypt command line arg
const char* arg_getgameinfo = "getgameinfo"; //RTC_Hijack: add command line arg to get a game's info

int find_arg(std::string arg, int& argc, char* argv[])
{
	arg = "--" + arg;
	for (int i = 1; i < argc; ++i)
		if (!strcmp(arg.c_str(), argv[i]))
			return i;
	return 0;
}

QCoreApplication* createApplication(int& argc, char* argv[])
{
	
	if (find_arg(arg_headless, argc, argv))
		return new headless_application(argc, argv);

#ifdef __linux__
	// set the DISPLAY variable in order to open web browsers
	if (qEnvironmentVariable("DISPLAY", "").isEmpty())
	{
		qputenv("DISPLAY", ":0");
	}

	// Set QT_AUTO_SCREEN_SCALE_FACTOR to 0. This value is deprecated but still seems to make problems on some distros
	if (!qEnvironmentVariable("QT_AUTO_SCREEN_SCALE_FACTOR", "").isEmpty())
	{
		qputenv("QT_AUTO_SCREEN_SCALE_FACTOR", "0");
	}
#endif

	bool use_high_dpi = true;

	const auto i_hdpi = find_arg(arg_high_dpi, argc, argv);
	if (i_hdpi)
	{
		const std::string cmp_str = "0";
		const auto i_hdpi_2 = (argc > (i_hdpi + 1)) ? (i_hdpi + 1) : 0;
		const auto high_dpi_setting = (i_hdpi_2 && !strcmp(cmp_str.c_str(), argv[i_hdpi_2])) ? "0" : "1";

		// Set QT_ENABLE_HIGHDPI_SCALING from environment. Defaults to cli argument, which defaults to 1.
		use_high_dpi = "1" == qEnvironmentVariable("QT_ENABLE_HIGHDPI_SCALING", high_dpi_setting);
	}

	// AA_EnableHighDpiScaling has to be set before creating a QApplication
	QApplication::setAttribute(use_high_dpi ? Qt::AA_EnableHighDpiScaling : Qt::AA_DisableHighDpiScaling);

	if (use_high_dpi)
	{
		// Set QT_SCALE_FACTOR_ROUNDING_POLICY from environment. Defaults to cli argument, which defaults to RoundPreferFloor.
		auto rounding_val = Qt::HighDpiScaleFactorRoundingPolicy::PassThrough;
		auto rounding_str = std::to_string(static_cast<int>(rounding_val));
		const auto i_rounding = find_arg(arg_rounding, argc, argv);

		if (i_rounding)
		{
			const auto i_rounding_2 = (argc > (i_rounding + 1)) ? (i_rounding + 1) : 0;

			if (i_rounding_2)
			{
				const auto arg_val = argv[i_rounding_2];
				const auto arg_len = std::strlen(arg_val);
				s64 rounding_val_cli = 0;

				if (!cfg::try_to_int64(&rounding_val_cli, arg_val, static_cast<int>(Qt::HighDpiScaleFactorRoundingPolicy::Unset), static_cast<int>(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough)))
				{
					std::cout << "The value " << arg_val << " for " << arg_rounding << " is not allowed. Please use a valid value for Qt::HighDpiScaleFactorRoundingPolicy.\n";
				}
				else
				{
					rounding_val = static_cast<Qt::HighDpiScaleFactorRoundingPolicy>(static_cast<int>(rounding_val_cli));
					rounding_str = std::to_string(static_cast<int>(rounding_val));
				}
			}
		}

		{
			rounding_str = qEnvironmentVariable("QT_SCALE_FACTOR_ROUNDING_POLICY", rounding_str.c_str()).toStdString();

			s64 rounding_val_final = 0;

			if (cfg::try_to_int64(&rounding_val_final, rounding_str, static_cast<int>(Qt::HighDpiScaleFactorRoundingPolicy::Unset), static_cast<int>(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough)))
			{
				rounding_val = static_cast<Qt::HighDpiScaleFactorRoundingPolicy>(static_cast<int>(rounding_val_final));
				rounding_str = std::to_string(static_cast<int>(rounding_val));
			}
			else
			{
				std::cout << "The value " << rounding_str << " for " << arg_rounding << " is not allowed. Please use a valid value for Qt::HighDpiScaleFactorRoundingPolicy.\n";
			}
		}
		QApplication::setHighDpiScaleFactorRoundingPolicy(rounding_val);
	}

	return new gui_application(argc, argv);
}



int main(int argc, char** argv)
{
#ifdef _WIN32
	ULONG64 intro_cycles{};
	QueryThreadCycleTime(GetCurrentThread(), &intro_cycles);
#elif defined(RUSAGE_THREAD)
	struct ::rusage intro_stats{};
	::getrusage(RUSAGE_THREAD, &intro_stats);
	const u64 intro_time = (intro_stats.ru_utime.tv_sec + intro_stats.ru_stime.tv_sec) * 1000000000ull + (intro_stats.ru_utime.tv_usec + intro_stats.ru_stime.tv_usec) * 1000ull;
#endif

	v128::use_fma = utils::has_fma3();

	s_argv0 = argv[0]; // Save for report_fatal_error

	// Only run RPCS3 to display an error
	if (int err_pos = find_arg(arg_error, argc, argv))
	{
		// Reconstruction of the error from multiple args
		std::string error;
		for (int i = err_pos + 1; i < argc; i++)
		{
			if (i > err_pos + 1)
				error += ' ';
			error += argv[i];
		}

		report_fatal_error(error);
	}

	const std::string lock_name = fs::get_cache_dir() + "RPCS3.buf";

	fs::file instance_lock;

	// True if an argument --updating found
	const bool is_updating = find_arg(arg_updating, argc, argv) != 0;

	// Keep trying to lock the file for ~2s normally, and for ~10s in the case of --updating
	for (u32 num = 0; num < (is_updating ? 500u : 100u) && !instance_lock.open(lock_name, fs::rewrite + fs::lock); num++)
	{
		std::this_thread::sleep_for(20ms);
	}

	if (!instance_lock)
	{
		if (fs::g_tls_error == fs::error::acces)
		{
			if (fs::exists(lock_name))
			{
				report_fatal_error("Another instance of RPCS3 is running. Close it or kill its process, if necessary.");
			}
			else
			{
				report_fatal_error("Cannot create RPCS3.log (access denied)."
#ifdef _WIN32
				"\nNote that RPCS3 cannot be installed in Program Files or similar directories with limited permissions."
#else
				"\nPlease, check RPCS3 permissions in '~/.config/rpcs3'."
#endif
				);
			}
		}
		else
		{
			report_fatal_error(fmt::format("Cannot create RPCS3.log (error %s)", fs::g_tls_error));
		}

		return 1;
	}

	std::unique_ptr<logs::listener> log_file;
	{
		// Check free space
		fs::device_stat stats{};
		if (!fs::statfs(fs::get_cache_dir(), stats) || stats.avail_free < 128 * 1024 * 1024)
		{
			report_fatal_error(fmt::format("Not enough free space (%f KB)", stats.avail_free / 1000000.));
			return 1;
		}

		// Limit log size to ~25% of free space
		log_file = logs::make_file_listener(fs::get_cache_dir() + "RPCS3.log", stats.avail_free / 4);
	}

	std::unique_ptr<logs::listener> log_pauser = std::make_unique<pause_on_fatal>();
	logs::listener::add(log_pauser.get());

	{
		const std::string firmware_version = utils::get_firmware_version();
		const std::string firmware_string  = firmware_version.empty() ? "" : (" | Firmware version: " + firmware_version);

		// Write initial message
		logs::stored_message ver;
		ver.m.ch  = nullptr;
		ver.m.sev = logs::level::always;
		ver.stamp = 0;
		ver.text  = fmt::format("RPCS3 v%s | %s%s\n%s", rpcs3::get_version().to_string(), rpcs3::get_branch(), firmware_string, utils::get_system_info());

		// Write OS version
		logs::stored_message os;
		os.m.ch  = nullptr;
		os.m.sev = logs::level::notice;
		os.stamp = 0;
		os.text = utils::get_OS_version();

		logs::set_init({std::move(ver), std::move(os)});
	}

#ifdef _WIN32
	sys_log.notice("Initialization times before main(): %fGc", intro_cycles / 1000000000.);
#elif defined(RUSAGE_THREAD)
	sys_log.notice("Initialization times before main(): %fs", intro_time / 1000000000.);
#endif

#ifdef __linux__
	struct ::rlimit rlim;
	rlim.rlim_cur = 4096;
	rlim.rlim_max = 4096;
	if (::setrlimit(RLIMIT_NOFILE, &rlim) != 0)
		std::fprintf(stderr, "Failed to set max open file limit (4096).");
	// Work around crash on startup on KDE: https://bugs.kde.org/show_bug.cgi?id=401637
	setenv( "KDE_DEBUG", "1", 0 );
#endif

	std::lock_guard qt_init(s_qt_init);

	// The constructor of QApplication eats the --style and --stylesheet arguments.
	// By checking for stylesheet().isEmpty() we could implicitly know if a stylesheet was passed,
	// but I haven't found an implicit way to check for style yet, so we naively check them both here for now.
	const bool use_cli_style = find_arg(arg_style, argc, argv) || find_arg(arg_stylesheet, argc, argv);

	QScopedPointer<QCoreApplication> app(createApplication(argc, argv));
	app->setApplicationVersion(QString::fromStdString(rpcs3::get_version().to_string()));
	app->setApplicationName("RPCS3");

	// Command line args
	QCommandLineParser parser;
	parser.setApplicationDescription("Welcome to RPCS3 command line.");
	parser.addPositionalArgument("(S)ELF", "Path for directly executing a (S)ELF");
	parser.addPositionalArgument("[Args...]", "Optional args for the executable");

	const QCommandLineOption help_option    = parser.addHelpOption();
	const QCommandLineOption version_option = parser.addVersionOption();
	parser.addOption(QCommandLineOption(arg_headless, "Run RPCS3 in headless mode."));
	parser.addOption(QCommandLineOption(arg_no_gui, "Run RPCS3 without its GUI."));
	parser.addOption(QCommandLineOption(arg_high_dpi, "Enables Qt High Dpi Scaling.", "enabled", "1"));
	parser.addOption(QCommandLineOption(arg_rounding, "Sets the Qt::HighDpiScaleFactorRoundingPolicy for values like 150% zoom.", "rounding", "4"));
	parser.addOption(QCommandLineOption(arg_styles, "Lists the available styles."));
	parser.addOption(QCommandLineOption(arg_style, "Loads a custom style.", "style", ""));
	parser.addOption(QCommandLineOption(arg_stylesheet, "Loads a custom stylesheet.", "path", ""));
	const QCommandLineOption config_option(arg_config, "Forces the emulator to use this configuration file.", "path", "");
	parser.addOption(config_option);
	parser.addOption(QCommandLineOption(arg_error, "For internal usage."));
	parser.addOption(QCommandLineOption(arg_updating, "For internal usage."));
	parser.addOption(QCommandLineOption(arg_decrypt, "Automatically decrypt a chosen self file.")); //RTC_Hijack: Describe decryption arg
	parser.addOption(QCommandLineOption(arg_getgameinfo, "Automatically output a game's serial number.")); //RTC_Hijack: Describe --getgameinfo
	parser.process(app->arguments());

	// Don't start up the full rpcs3 gui if we just want the version or help.
	if (parser.isSet(version_option) || parser.isSet(help_option))
		return 0;

	if (parser.isSet(arg_styles))
	{
#ifdef _WIN32
		if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole())
			[[maybe_unused]] const auto con_out = freopen("CONOUT$", "w", stdout);
#endif
		for (const auto& style : QStyleFactory::keys())
			std::cout << "\n" << style.toStdString();

		return 0;
	}

	if (auto gui_app = qobject_cast<gui_application*>(app.data()))
	{
		gui_app->setAttribute(Qt::AA_UseHighDpiPixmaps);
		gui_app->setAttribute(Qt::AA_DisableWindowContextHelpButton);
		gui_app->setAttribute(Qt::AA_DontCheckOpenGLContextThreadAffinity);

		gui_app->SetShowGui(!parser.isSet(arg_no_gui));
		gui_app->SetUseCliStyle(use_cli_style);
		gui_app->Init();
	}
	else if (auto headless_app = qobject_cast<headless_application*>(app.data()))
	{
		s_headless = true;
		headless_app->Init();
	}

#ifdef _WIN32
	// Set 0.5 msec timer resolution for best performance
	// - As QT5 timers (QTimer) sets the timer resolution to 1 msec, override it here.
	// - Don't bother "unsetting" the timer resolution after the emulator stops as QT5 will still require the timer resolution to be set to 1 msec.
	ULONG min_res, max_res, orig_res, new_res;
	if (NtQueryTimerResolution(&min_res, &max_res, &orig_res) == 0)
	{
		NtSetTimerResolution(max_res, TRUE, &new_res);
	}
#endif

	std::string config_override_path;

	if (parser.isSet(arg_config))
	{
		config_override_path = parser.value(config_option).toStdString();

		if (!fs::is_file(config_override_path))
		{
			report_fatal_error(fmt::format("No config file found: %s", config_override_path));
			return 0;
		}

		Emu.SetConfigOverride(config_override_path);
	}

	for (const auto& opt : parser.optionNames())
	{
		sys_log.notice("Option passed via command line: %s = %s", opt.toStdString(), parser.value(opt).toStdString());
	}

	if (const QStringList args = parser.positionalArguments(); !args.isEmpty())
	{	//RTC_Hijack: Implement arguments for file decryption and gameinfo getting
		if (find_arg(arg_decrypt, argc, argv))
		{
			std::string path;
			path = sstr(QFileInfo(args.at(0)).absoluteFilePath());


			fs::file selectedElf(path);

			if (selectedElf.read<u32>() == "SCE\0"_u32)
			{
				Emu.SetForceBoot(true);
				Emu.Stop();
				selectedElf = decrypt_self(std::move(selectedElf));
				if (fs::file new_file{path, fs::rewrite})
				{
					new_file.write(selectedElf.to_string());
				}

			return 0;
			}
		}
		else if (find_arg(arg_getgameinfo, argc, argv))
		{
			//god my code is ugly but hopefully it'll do
			std::string dir = (sstr(QFileInfo(args.at(0)).absoluteFilePath())); //assume the user will pick the game FOLDER (which includes PS3_GAME and the SFB file), not its eboot
			sys_log.notice("Game Directory: %s", dir);
			const std::string sfo_dir = Emulator::GetSfoDirFromGamePath(dir, Emu.GetUsr());
			const fs::file sfo_file(sfo_dir + "/PARAM.SFO");
			if (!sfo_file)
			{
				sys_log.notice("ERROR: Could note find SFO file! Attempted filename location: %s", (sfo_dir +"/PARAM.SFO"));
				return 0;
			}
			sys_log.notice("SFO file location: %s", (sfo_dir + "/PARAM.SFO"));
			GameInfo game;
			const auto psf = psf::load_object(sfo_file);
			game.path                = dir;
			game.icon_path           = sfo_dir + "/ICON0.PNG";
			game.serial              = std::string(psf::get_string(psf, "TITLE_ID", ""));
			game.name                = std::string(psf::get_string(psf, "TITLE"));
			game.app_ver             = std::string(psf::get_string(psf, "APP_VER"));
			game.version             = std::string(psf::get_string(psf, "VERSION"));
			game.category            = std::string(psf::get_string(psf, "CATEGORY"));
			game.fw                  = std::string(psf::get_string(psf, "PS3_SYSTEM_VER"));
			game.parental_lvl        = psf::get_integer(psf, "PARENTAL_LEVEL", 0);
			game.resolution          = psf::get_integer(psf, "RESOLUTION", 0);
			game.sound_format        = psf::get_integer(psf, "SOUND_FORMAT", 0);
			game.bootable            = psf::get_integer(psf, "BOOTABLE", 0);
			game.attr                = psf::get_integer(psf, "ATTRIBUTE", 0);
			std::string serialnumber = game.serial;
			/*std::ofstream out(dir + "/gameinfo.txt");
			sys_log.notice("Attempted txt file location: %s", (dir + "/gameinfo.txt"));
			std::string outputText = "Game name: " + game.name + "\nGame serial: " + game.serial + "\nGame version: " + game.version;
			out << outputText;*/
			fs::file outputText{(dir + "/gameinfo.txt"), fs::create};
			outputText.open();
			sys_log.notice("Attempted txt file location: %s", (dir + "/gameinfo.txt"));
			outputText.write("Game name: " + game.name + "\nGame serial: " + game.serial + "\nGame version: " + game.version);

			return 0;
		}else
		//RTC_Hijack end
		{
			sys_log.notice("Booting application from command line: %s", args.at(0).toStdString());

			// Propagate command line arguments
			std::vector<std::string> argv;

			if (args.length() > 1)
			{
				argv.emplace_back();

				for (int i = 1; i < args.length(); i++)
				{
					const std::string arg = args[i].toStdString();
					argv.emplace_back(arg);
					sys_log.notice("Optional command line argument %d: %s", i, arg);
				}
			}

			// Ugly workaround
			QTimer::singleShot(2, [config_override_path, path = sstr(QFileInfo(args.at(0)).absoluteFilePath()), argv = std::move(argv)]() mutable {
				Emu.argv = std::move(argv);
				Emu.SetForceBoot(true);
				Emu.BootGame(path, "", true);
			});
		}

	}

	// run event loop (maybe only needed for the gui application)
	return app->exec();
}
