﻿#include <QTime>
#include <thread>

#include "gammactl.h"
#include "defs.h"
#include "utils.h"
#include "cfg.h"

GammaCtl::GammaCtl()
{
	// If auto brightness is on, start at max brightness
	if (cfg["auto_br"])
		cfg["brightness"] = brt_slider_steps;

	// If auto temp is on, start at max temp for a smooth transition
	if (cfg["auto_temp"])
		cfg["temp_step"] = 0;

	setGamma(cfg["brightness"], cfg["temp_step"]);
}

void GammaCtl::setWindow(MainWindow *w)
{
	this->wnd = w;
}

void GammaCtl::start()
{
	if (threads.empty()) {
		threads.push_back(std::thread([this] { adjustTemperature(); }));
		threads.push_back(std::thread([this] { captureScreen(); }));
		LOGD << "Gamma control started";
	}
}

void GammaCtl::stop()
{
	quit = true;
	temp_cv.notify_one();
	ss_cv.notify_one();

	if (!threads.empty()) {
		for (auto &t : threads)
			t.join();

		threads.clear();
	}

	LOGD << "Gamma control stopped";
}

void GammaCtl::notify_temp(bool force)
{
	force_temp_change = force;
	temp_cv.notify_one();
}

void GammaCtl::notify_ss()
{
	ss_cv.notify_one();
}

void GammaCtl::captureScreen()
{
	LOGV << "captureScreen() start";

	std::thread br_thr([&] { adjustBrightness(); });

	const uint64_t screen_res = getResolution();
	const uint64_t buf_sz     = screen_res * 4;

	if (windows && !initDXGI()) {
		LOGE << "DXGI init failed. Using GDI instead.";
		wnd->setPollingRange(1000, 5000);
	}

	LOGD << "Screen res: " << screen_res << ", buffer size: " << buf_sz;

	// Buffer to store screen pixels
	std::vector<uint8_t> buf;

	std::mutex m;

	int img_delta = 0;

	bool force = false;

	int
	prev_img_br = 0,
	prev_min    = 0,
	prev_max    = 0,
	prev_offset = 0;

	while (true) {
		{
			std::unique_lock<std::mutex> lock(m);

			ss_cv.wait(lock, [&] {
				return cfg["auto_br"] || quit;
			});
		}

		if (quit)
			break;

		if (cfg["auto_br"]) {
			buf.resize(buf_sz);
			force = true;
		} else {
			buf.clear();
			buf.shrink_to_fit();
			continue;
		}

		while (cfg["auto_br"] && !quit) {
			LOGV << "Taking screenshot";
			getSnapshot(buf);

			const int img_br = calcBrightness(buf);
			img_delta += abs(prev_img_br - img_br);

			if (img_delta > cfg["threshold"] || force) {
				img_delta = 0;
				force = false;

				{
					std::lock_guard lock(br_mtx);
					this->ss_brightness = img_br;
					br_needs_change = true;
				}

				br_cv.notify_one();
			}

			if (cfg["min_br"] != prev_min || cfg["max_br"] != prev_max || cfg["offset"] != prev_offset)
				force = true;

			prev_img_br = img_br;
			prev_min    = cfg["min_br"];
			prev_max    = cfg["max_br"];
			prev_offset = cfg["offset"];
		}

		buf.clear();
		buf.shrink_to_fit();
	}

	LOGV << "Screenshot loop ended. Notifying adjustBrightness";

	{
		std::lock_guard<std::mutex> lock (br_mtx);
		br_needs_change = true;
	}

	br_cv.notify_one();
	br_thr.join();

	LOGV << "adjustBrightness joined.";
}

void GammaCtl::adjustBrightness()
{
	using namespace std::this_thread;
	using namespace std::chrono;

	while (true) {
		int img_br;

		{
			std::unique_lock<std::mutex> lock(br_mtx);

			br_cv.wait(lock, [&] {
				return br_needs_change;
			});

			if (quit)
				break;

			br_needs_change = false;
			img_br = this->ss_brightness;
		}

		int target = brt_slider_steps - int(remap(img_br, 0, 255, 0, brt_slider_steps)) + cfg["offset"].get<int>();
		target = std::clamp(target, cfg["min_br"].get<int>(), cfg["max_br"].get<int>());

		if (target == cfg["brightness"]) {
			LOGD << "Brt already at target (" << target << ')';
			continue;
		}

		const int start         = cfg["brightness"];
		const int end           = target;
		double duration_s       = cfg["speed"];
		const int FPS           = cfg["brt_fps"];
		const double iterations = FPS * duration_s;
		const int distance      = end - start;
		const double time_incr  = duration_s / iterations;

		double time = 0;

		LOGD << "(" << start << "->" << end << ')';

		while (cfg["brightness"] != target && !br_needs_change && cfg["auto_br"] && !quit) {
			time += time_incr;
			cfg["brightness"] = std::round(easeOutExpo(time, start, distance, duration_s));
			wnd->setBrtSlider(cfg["brightness"]);
			sleep_for(milliseconds(1000 / FPS));
		}

		LOGD << "(" << start << "->" << end << ") done";
	}
}

/**
 * The temperature is adjusted in two steps.
 * The first one is for quickly catching up to the proper temperature when:
 * - time is checked sometime after the start time
 * - the system wakes up
 * - temperature settings change
 */
void GammaCtl::adjustTemperature()
{
	using namespace std::this_thread;
	using namespace std::chrono;
	using namespace std::chrono_literals;

	QTime start_time;
	QTime end_time;

	const auto setTime = [] (QTime &t, const std::string &time_str) {
		const auto start_h = time_str.substr(0, 2);
		const auto start_m = time_str.substr(3, 2);
		t = QTime(std::stoi(start_h), std::stoi(start_m));
	};

	const auto updateInterval = [&] {
		std::string t_start = cfg["time_start"];
		const int h = std::stoi(t_start.substr(0, 2));
		const int m = std::stoi(t_start.substr(3, 2));

		const int adapt_time_s = cfg["temp_speed"].get<double>() * 60;
		const QTime adapted_start = QTime(h, m).addSecs(-adapt_time_s);

		setTime(start_time, adapted_start.toString().toStdString());
		setTime(end_time, cfg["time_end"]);
	};

	updateInterval();

	bool needs_change = cfg["auto_temp"];

	convar     clock_cv;
	std::mutex clock_mtx;
	std::mutex temp_mtx;

	std::thread clock ([&] {
		while (true) {
			{
				std::unique_lock<std::mutex> lk(clock_mtx);
				clock_cv.wait_until(lk, system_clock::now() + 60s, [&] {
					return quit;
				});
			}

			if (quit)
				break;

			if (!cfg["auto_temp"])
				continue;

			{
				std::lock_guard<std::mutex> lock(temp_mtx);
				needs_change = true; // @TODO: Should be false if the state hasn't changed
			}

			temp_cv.notify_one();
		}
	});

	bool first_step_done = false;

	while (true) {
		{
			std::unique_lock<std::mutex> lock(temp_mtx);

			temp_cv.wait(lock, [&] {
				return needs_change || first_step_done || force_temp_change || quit;
			});

			if (quit)
				break;

			if (force_temp_change) {
				updateInterval();
				force_temp_change = false;
				first_step_done = false;
			}

			needs_change = false;
		}

		if (!cfg["auto_temp"])
			continue;


		int    target_temp = cfg["temp_low"]; // Temperature target in Kelvin
		double duration_s  = 2;               // Seconds it takes to reach it

		const double    adapt_time_s = cfg["temp_speed"].get<double>() * 60;
		const QDateTime cur_datetime = QDateTime::currentDateTime();
		const QTime     cur_time     = cur_datetime.time();

		if ((cur_time >= start_time) || (cur_time < end_time)) {

			QDateTime start_datetime(cur_datetime.date(), start_time);

			/* If we are earlier than both sunset and sunrise times
			 * we need to count from yesterday. */
			if (cur_time < end_time)
				start_datetime = start_datetime.addDays(-1);

			int secs_from_start = start_datetime.secsTo(cur_datetime);

			LOGD << "secs_from_start: " << secs_from_start << " adapt_time_s: " << adapt_time_s;

			if (secs_from_start > adapt_time_s)
				secs_from_start = adapt_time_s;

			if (!first_step_done) {
				target_temp = remap(secs_from_start, 0, adapt_time_s, cfg["temp_high"], cfg["temp_low"]);
			} else {
				duration_s = adapt_time_s - secs_from_start;
				if (duration_s < 2)
					duration_s = 2;
			}
		} else {
			target_temp = cfg["temp_high"];
		}

		LOGD << "Duration: " << duration_s / 60 << " min";

		int cur_step    = cfg["temp_step"];
		int target_step = int(remap(target_temp, min_temp_kelvin, max_temp_kelvin, temp_slider_steps, 0));

		if (cur_step == target_step) {
			LOGD << "Temp already at target (" << target_temp << " K)";
			first_step_done = false;
			continue;
		}

		LOGD << "Temp target: " << target_temp << " K";

		const int    FPS        = cfg["temp_fps"];
		const double iterations = FPS * duration_s;
		const double time_incr  = duration_s / iterations;
		const int    distance   = target_step - cur_step;

		double time = 0;

		LOGD << "(" << cur_step << "->" << target_step << ')';

		while (cfg["temp_step"] != target_step && cfg["auto_temp"]) {
			if (force_temp_change || quit)
				break;

			time += time_incr;
			cfg["temp_step"] = int(easeInOutQuad(time, cur_step, distance, duration_s));
			wnd->setTempSlider(cfg["temp_step"]);
			sleep_for(milliseconds(1000 / FPS));
		}

		first_step_done = true;

		LOGD << "(" << cur_step << "->" << target_step << ") done";
	}

	LOGV << "Temp loop ended. Notifying clock thread";

	clock_cv.notify_one();
	clock.join();

	LOGV << "Clock thread joined.";
}