/*
   Copyright 2023      Leil Storage OÜ

   This file is part of SaunaFS.

   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS. If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <gtest/gtest.h>

#include "common/observable_property.h"

TEST(SignalSlotTest, BasicSignalWithNoParameters) {
	Signal<> signal;
	int callCount = 0;

	signal.connect([&callCount]() { callCount++; });

	EXPECT_EQ(callCount, 0);
	signal.emit();
	EXPECT_EQ(callCount, 1);
	signal.emit();
	EXPECT_EQ(callCount, 2);
}

TEST(SignalSlotTest, SignalWithSingleParameter) {
	Signal<std::string> signal;
	std::string receivedMessage;

	signal.connect([&receivedMessage](const std::string &msg) { receivedMessage = msg; });

	signal.emit("Hello World");
	EXPECT_EQ(receivedMessage, "Hello World");
}

TEST(SignalSlotTest, SignalWithMultipleParameters) {
	Signal<int, const std::string &, bool> signal;
	int receivedInt = 0;
	std::string receivedString;
	bool receivedBool = false;

	signal.connect([&](int i, const std::string &s, bool b) {
		receivedInt = i;
		receivedString = s;
		receivedBool = b;
	});

	signal.emit(42, "test", true);
	EXPECT_EQ(receivedInt, 42);
	EXPECT_EQ(receivedString, "test");
	EXPECT_TRUE(receivedBool);
}

TEST(SignalSlotTest, MultipleSlots) {
	Signal<int> signal;
	int slot1Value = 0;
	int slot2Value = 0;

	signal.connect([&slot1Value](int value) { slot1Value = value * 2; });

	signal.connect([&slot2Value](int value) { slot2Value = value + 10; });

	signal.emit(5);
	EXPECT_EQ(slot1Value, 10);
	EXPECT_EQ(slot2Value, 15);
}

TEST(SignalSlotTest, ObservablePropertyBasic) {
	ObservableProperty<int> property("test_int", 100);

	EXPECT_EQ(property.getValue(), 100);

	int oldValue = -1;
	int newValue = -1;
	property.connect([&](const int &oldVal, const int &newVal) {
		oldValue = oldVal;
		newValue = newVal;
	});

	property.setValue(200);
	EXPECT_EQ(property.getValue(), 200);
	EXPECT_EQ(oldValue, 100);
	EXPECT_EQ(newValue, 200);
}

TEST(SignalSlotTest, ObservablePropertySimpleSlot) {
	ObservableProperty<std::string> property("test_string", "initial");
	int changeCount = 0;

	property.connectSimple([&changeCount]() { changeCount++; });

	property.setValue("changed1");
	EXPECT_EQ(changeCount, 1);
	property.setValue("changed2");
	EXPECT_EQ(changeCount, 2);
}

TEST(SignalSlotTest, ObservablePropertyMultipleObservers) {
	ObservableProperty<bool> property("test_bool", false);
	int observer1Count = 0;
	int observer2Count = 0;

	property.connect([&observer1Count](const bool &, const bool &) { observer1Count++; });

	property.connectSimple([&observer2Count]() { observer2Count++; });

	property.setValue(true);
	EXPECT_EQ(observer1Count, 1);
	EXPECT_EQ(observer2Count, 1);

	property.setValue(false);
	EXPECT_EQ(observer1Count, 2);
	EXPECT_EQ(observer2Count, 2);
}

TEST(SignalSlotTest, SignalClearAndSize) {
	Signal<> signal;
	int callCount = 0;

	EXPECT_EQ(signal.size(), 0UL);

	signal.connect([&callCount]() { callCount++; });
	signal.connect([&callCount]() { callCount++; });
	EXPECT_EQ(signal.size(), 2UL);

	signal.emit();
	EXPECT_EQ(callCount, 2);

	signal.clear();
	EXPECT_EQ(signal.size(), 0UL);

	signal.emit();
	EXPECT_EQ(callCount, 2);  // Should not change after clear
}

TEST(SignalSlotTest, ComplexTypeSignal) {
	struct TestData {
		int id;
		std::string name;
		bool operator==(const TestData &other) const {
			return id == other.id && name == other.name;
		}
	};

	Signal<const TestData &> signal;
	TestData receivedData{.id = 0, .name = ""};

	signal.connect([&receivedData](const TestData &data) { receivedData = data; });

	TestData testData{.id = 123, .name = "test_name"};
	signal.emit(testData);

	EXPECT_EQ(receivedData.id, 123);
	EXPECT_EQ(receivedData.name, "test_name");
}
