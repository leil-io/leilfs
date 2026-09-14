/*
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ

   This file is part of LeilFS.

   LeilFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   LeilFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with LeilFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"
#include "common/token_bucket.h"

#include <algorithm>

#include "common/massert.h"

void TokenBucket::reconfigure(SteadyTimePoint now, double rate, double budgetCeil) {
	updateBudget(now);
	rate_ = rate;
	budgetCeil_ = budgetCeil;
}

void TokenBucket::reconfigure(SteadyTimePoint now, double rate, double budgetCeil, double budget) {
	reconfigure(now, rate, budgetCeil);
	budget_ = budget;
}

double TokenBucket::rate() const {
	return rate_;
}

double TokenBucket::budgetCeil() const {
	return budgetCeil_;
}

uint64_t TokenBucket::attempt(SteadyTimePoint now, double cost) {
	sassert(cost > 0);
	updateBudget(now);
	const uint64_t result = std::min(cost, budget_);
	budget_ -= result;
	return result;
}

void TokenBucket::updateBudget(SteadyTimePoint now) {
	verifyClockSteadiness(now);
	std::chrono::duration<double, std::nano> time(now - prevTime_);
	prevTime_ = now;
	int64_t time_ns = time.count();
	budget_ += rate_ * time_ns / (1000 * 1000 * 1000.0);
	if (budget_ > budgetCeil_) {
		budget_ = budgetCeil_;
	}
}

void TokenBucket::verifyClockSteadiness(SteadyTimePoint now) {
	sassert(now >= prevTime_);
}
