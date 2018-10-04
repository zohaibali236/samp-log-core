#include <algorithm>
#include <fstream>
#include <iostream>
#include <ctime>
#include <unordered_set>

#ifdef WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <Windows.h>
#else
#  include <sys/stat.h>
#endif

#include "LogManager.hpp"
#include "Logger.hpp"
#include "SampConfigReader.hpp"
#include "LogConfigReader.hpp"
#include "crashhandler.hpp"
#include "amx/amx2.h"

#include <fmt/format.h>
#include <fmt/time.h>
#include <fmt/color.h>

using samplog::LogLevel;


const char *GetLogLevelAsString(LogLevel level)
{
	switch (level)
	{
		case LogLevel::DEBUG:
			return "DEBUG";
		case LogLevel::INFO:
			return "INFO";
		case LogLevel::WARNING:
			return "WARNING";
		case LogLevel::ERROR:
			return "ERROR";
		case LogLevel::FATAL:
			return "FATAL";
		case LogLevel::VERBOSE:
			return "VERBOSE";
	}
	return "<unknown>";
}

fmt::rgb GetLogLevelColor(LogLevel level)
{
	switch (level)
	{
	case LogLevel::DEBUG:
		return fmt::color::green;
	case LogLevel::INFO:
		return fmt::color::royal_blue;
	case LogLevel::WARNING:
		return fmt::color::orange;
	case LogLevel::ERROR:
		return fmt::color::red;
	case LogLevel::FATAL:
		return fmt::color::red;
	case LogLevel::VERBOSE:
		return fmt::color::white_smoke;
	}
	return fmt::color::white;
}

void WriteCallInfoString(Message_t const &msg, fmt::memory_buffer &log_string)
{
	if (!msg->call_info.empty())
	{
		fmt::format_to(log_string, " (");
		bool first = true;
		for (auto const &ci : msg->call_info)
		{
			if (!first)
				fmt::format_to(log_string, " -> ");
			fmt::format_to(log_string, "{:s}:{:d}", ci.file, ci.line);
			first = false;
		}
		fmt::format_to(log_string, ")");
	}
}

void CreateFolder(std::string foldername)
{
#ifdef WIN32
	std::replace(foldername.begin(), foldername.end(), '/', '\\');
	CreateDirectoryA(foldername.c_str(), NULL);
#else
	std::replace(foldername.begin(), foldername.end(), '\\', '/');
	mkdir(foldername.c_str(), ACCESSPERMS);
#endif
}

void EnsureTerminalColorSupport()
{
	static bool enabled = false;
	if (enabled)
		return;

#ifdef WIN32
	HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
	if (console == INVALID_HANDLE_VALUE)
		return;

	DWORD console_opts;
	if (!GetConsoleMode(console, &console_opts))
		return;

	if (!SetConsoleMode(console, console_opts | ENABLE_VIRTUAL_TERMINAL_PROCESSING))
		return;
#endif
	enabled = true;
}


LogManager::LogManager() :
	m_ThreadRunning(true),
	m_DateTimeFormat("{:%x %X}")
{
	crashhandler::Install();

	std::string cfg_time_format;
	if (SampConfigReader::Get()->GetVar("logtimeformat", cfg_time_format))
	{
		//delete brackets
		size_t pos = 0;
		while ((pos = cfg_time_format.find_first_of("[]()")) != std::string::npos)
			cfg_time_format.erase(pos, 1);

		m_DateTimeFormat = "{:" + cfg_time_format + "}";
		// quickly test out the format string
		// will assert if invalid and on Windows
		fmt::format(m_DateTimeFormat, fmt::localtime(std::time(nullptr)));
	}

	LogConfigReader::Get()->Initialize();

	CreateFolder("logs");

	m_WarningLog.open("logs/warnings.log");
	m_ErrorLog.open("logs/errors.log");
	m_FatalLog.open("logs/fatals.log");

	m_Thread = new std::thread(std::bind(&LogManager::Process, this));
}

LogManager::~LogManager()
{
	{
		std::lock_guard<std::mutex> lg(m_QueueMtx);
		m_ThreadRunning = false;
	}
	m_QueueNotifier.notify_one();
	m_Thread->join();
	delete m_Thread;

	m_WarningLog.close();
	m_ErrorLog.close();
	m_FatalLog.close();
}

void LogManager::RegisterLogger(Logger *logger)
{
	std::lock_guard<std::mutex> lg(m_LoggersMutex);
	m_Loggers.emplace(logger->GetModuleName(), logger);
}

void LogManager::UnregisterLogger(Logger *logger)
{
	bool is_last = false;
	{
		std::lock_guard<std::mutex> lg(m_LoggersMutex);
		m_Loggers.erase(logger->GetModuleName());
		is_last = (m_Loggers.size() == 0);
	}
	if (is_last) //last logger
		CSingleton::Destroy();
}

void LogManager::QueueLogMessage(Message_t &&msg)
{
	{
		std::lock_guard<std::mutex> lg(m_QueueMtx);
		m_LogMsgQueue.push(std::move(msg));
	}
	m_QueueNotifier.notify_one();
}

void LogManager::Process()
{
	std::unique_lock<std::mutex> lk(m_QueueMtx);
	std::unordered_set<std::string> hashed_modules;

	do
	{
		m_QueueNotifier.wait(lk);
		while (!m_LogMsgQueue.empty())
		{
			Message_t msg = std::move(m_LogMsgQueue.front());
			m_LogMsgQueue.pop();

			//manually unlock mutex
			//the whole write-to-file code below has no need to be locked with the
			//message queue mutex; while writing to the log file, new messages can
			//now be queued
			lk.unlock();

			const string &modulename = msg->log_module;
			if (hashed_modules.count(modulename) == 0)
			{
				//create possibly non-existing folders before opening log file
				size_t pos = 0;
				while ((pos = modulename.find('/', pos)) != std::string::npos)
				{
					CreateFolder("logs/" + modulename.substr(0, pos++));
				}

				hashed_modules.insert(modulename);
			}

			std::string timestamp;
			std::time_t now_c = std::chrono::system_clock::to_time_t(msg->timestamp);
			timestamp = fmt::format(m_DateTimeFormat, fmt::localtime(now_c));

			const char *loglevel_str = GetLogLevelAsString(msg->loglevel);

			// build log string
			fmt::memory_buffer log_string_buf;

			fmt::format_to(log_string_buf, "{:s}", msg->text);
			WriteCallInfoString(msg, log_string_buf);

			std::string const log_string = fmt::to_string(log_string_buf);

			//default logging
			std::ofstream logfile("logs/" + modulename + ".log",
				std::ofstream::out | std::ofstream::app);
			logfile <<
				"[" << timestamp << "] " <<
				"[" << loglevel_str << "] " <<
				log_string << '\n' << std::flush;


			//per-log-level logging
			std::ofstream *loglevel_file = nullptr;
			if (msg->loglevel == LogLevel::WARNING)
				loglevel_file = &m_WarningLog;
			else if (msg->loglevel == LogLevel::ERROR)
				loglevel_file = &m_ErrorLog;
			else if (msg->loglevel == LogLevel::FATAL)
				loglevel_file = &m_FatalLog;

			if (loglevel_file != nullptr)
			{
				(*loglevel_file) <<
					"[" << timestamp << "] " <<
					"[" << modulename << "] " <<
					log_string << '\n' << std::flush;
			}

			LogConfig log_config;
			LogConfigReader::Get()->GetLoggerConfig(modulename, log_config);
			auto const &level_config = LogConfigReader::Get()->GetLogLevelConfig(msg->loglevel);

			if (log_config.PrintToConsole || level_config.PrintToConsole)
			{
				if (LogConfigReader::Get()->GetGlobalConfig().EnableColors)
				{
					EnsureTerminalColorSupport();

					fmt::print("[");
					fmt::print(fmt::rgb(255, 255, 150), timestamp);
					fmt::print("] [");
					fmt::print(fmt::color::sandy_brown, modulename);
					fmt::print("] [");
					auto loglevel_color = GetLogLevelColor(msg->loglevel);
					if (msg->loglevel == LogLevel::FATAL)
						fmt::print(fmt::color::white, loglevel_color, loglevel_str);
					else
						fmt::print(loglevel_color, loglevel_str);
					fmt::print("] {:s}\n", log_string);
				}
				else
				{
					fmt::print("[{:s}] [{:s}] [{:s}] {:s}\n",
						timestamp, modulename, loglevel_str, log_string);
				}
			}

			//lock the log message queue again (because while-condition and cv.wait)
			lk.lock();
		}
	} while (m_ThreadRunning);
}
