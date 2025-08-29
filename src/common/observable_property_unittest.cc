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
#include <cstdint>

#include "common/observable_property.h"
#include "common/type_defs.h"

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

// ObservableIntegralProperty tests

TEST(SignalSlotTest, ObservableIntegralPropertyIncrementDecrement) {
	ObservableIntegralProperty<int> property("test_int", 10);

	EXPECT_EQ(property.getValue(), 10);

	property.increment();
	EXPECT_EQ(property.getValue(), 11);

	property.decrement();
	EXPECT_EQ(property.getValue(), 10);

	property.increment(5);
	EXPECT_EQ(property.getValue(), 15);

	property.decrement(3);
	EXPECT_EQ(property.getValue(), 12);
}

TEST(SignalSlotTest, MemberFunctionSlot_NoArgs_NonConst_And_Const) {
	struct NoArgsHandler {
		int nonConstCount = 0;
		mutable int constCount = 0;

		void handle() { nonConstCount++; }
		void handleConst() const { constCount++; }
	} handler;

	Signal<> signal;

	// Connect non-const member function
	signal.connect(&handler, &NoArgsHandler::handle);

	signal.emit();
	EXPECT_EQ(handler.nonConstCount, 1);
	EXPECT_EQ(handler.constCount, 0);

	// Connect const member function (requires pointer to const object)
	const NoArgsHandler *constHandlerPtr = &handler;
	signal.connect(constHandlerPtr, &NoArgsHandler::handleConst);

	EXPECT_EQ(handler.nonConstCount, 1);
	EXPECT_EQ(handler.constCount, 0);

	signal.emit();
	EXPECT_EQ(handler.nonConstCount, 2);
	EXPECT_EQ(handler.constCount, 1);

	signal.connect(&handler, &NoArgsHandler::handle);

	signal.emit();
	EXPECT_EQ(handler.nonConstCount, 4);  // Called twice now
	EXPECT_EQ(handler.constCount, 2);
}

TEST(SignalSlotTest, MemberFunctionSlot_WithArgs_NonConst_And_Const) {
	struct NonConstReceiver {
		int i = 0;
		std::string s;
		bool b = false;
		void onArgs(int inputInt, const std::string &inputString, bool inputBool) {
			i = inputInt;
			s = inputString;
			b = inputBool;
		}

		void onArbitraryChangingArgs(int inputInt, const std::string &inputString, bool inputBool) {
			i = inputInt * 2;
			s = inputString + "_modified";
			b = !inputBool;
		}
	} nonConstReceiver;

	struct ConstReceiver {
		mutable int i = 0;
		mutable std::string s;
		mutable bool b = false;
		void onArgsConst(int inputInt, const std::string &inputString, bool inputBool) const {
			i = inputInt;
			s = inputString;
			b = inputBool;
		}
	} constReceiver;

	Signal<int, const std::string &, bool> signal;

	signal.connect(&nonConstReceiver, &NonConstReceiver::onArgs);
	const ConstReceiver *constReceiverPtr = &constReceiver;
	signal.connect(constReceiverPtr, &ConstReceiver::onArgsConst);

	int expectedInt = 7;
	std::string expectedStr = "seven";
	bool expectedBool = true;

	signal.emit(expectedInt, expectedStr, expectedBool);

	EXPECT_EQ(nonConstReceiver.i, expectedInt);
	EXPECT_EQ(nonConstReceiver.s, expectedStr);
	EXPECT_TRUE(nonConstReceiver.b);

	EXPECT_EQ(constReceiver.i, expectedInt);
	EXPECT_EQ(constReceiver.s, expectedStr);
	EXPECT_TRUE(constReceiver.b);

	expectedInt = 3;
	expectedStr = "three";

	signal.connect(&nonConstReceiver, &NonConstReceiver::onArbitraryChangingArgs);
	signal.emit(expectedInt, "three", expectedBool);

	EXPECT_EQ(nonConstReceiver.i, expectedInt * 2);
	EXPECT_EQ(nonConstReceiver.s, expectedStr + "_modified");
	EXPECT_EQ(nonConstReceiver.b, !expectedBool);
}

TEST(SignalSlotTest, ObservableProperty_ConnectMember_ByConstRef_NonConstAndConst) {
	struct Observer {
		int lastOld = 0;
		int lastNew = 0;
		void onChange(const int &oldValue, const int &newValue) {
			lastOld = oldValue;
			lastNew = newValue;
		}
	} obs;

	struct ConstObserver {
		mutable int lastOld = 0;
		mutable int lastNew = 0;
		void onChangeConst(const int &oldValue, const int &newValue) const {
			lastOld = oldValue;
			lastNew = newValue;
		}
	} cobs;

	ObservableProperty<int> prop("member_ref_int", 1);

	prop.connect(&obs, &Observer::onChange);
	const ConstObserver *pcobs = &cobs;
	prop.connect(pcobs, &ConstObserver::onChangeConst);

	const int kUpdatedValue = 10;
	prop.setValue(kUpdatedValue);

	EXPECT_EQ(obs.lastOld, 1);
	EXPECT_EQ(obs.lastNew, kUpdatedValue);
	EXPECT_EQ(cobs.lastOld, 1);
	EXPECT_EQ(cobs.lastNew, kUpdatedValue);

	const int kSomeOtherValue = 20;
	prop.setValue(kSomeOtherValue);

	EXPECT_EQ(obs.lastOld, kUpdatedValue);
	EXPECT_EQ(obs.lastNew, kSomeOtherValue);
	EXPECT_EQ(cobs.lastOld, kUpdatedValue);
	EXPECT_EQ(cobs.lastNew, kSomeOtherValue);
}

TEST(SignalSlotTest, ObservableProperty_ConnectMember_ByValue_NonConstAndConst) {
	struct ObserverV {
		int lastOld = 0;
		int lastNew = 0;
		void onChange(int oldValue, int newValue) {
			lastOld = oldValue;
			lastNew = newValue;
		}
	} obs;

	struct ConstObserverV {
		mutable int lastOld = 0;
		mutable int lastNew = 0;
		void onChangeConst(int oldValue, int newValue) const {
			lastOld = oldValue;
			lastNew = newValue;
		}
	} cobs;

	ObservableProperty<int> prop("member_val_int", 5);

	prop.connect(&obs, &ObserverV::onChange);
	const ConstObserverV *pcobs = &cobs;
	prop.connect(pcobs, &ConstObserverV::onChangeConst);

	const int kNewValue = 8;
	prop.setValue(kNewValue);

	EXPECT_EQ(obs.lastOld, 5);
	EXPECT_EQ(obs.lastNew, kNewValue);
	EXPECT_EQ(cobs.lastOld, 5);
	EXPECT_EQ(cobs.lastNew, kNewValue);

	const int kAnotherValue = 15;
	prop.setValue(kAnotherValue);

	EXPECT_EQ(obs.lastOld, kNewValue);
	EXPECT_EQ(obs.lastNew, kAnotherValue);
	EXPECT_EQ(cobs.lastOld, kNewValue);
	EXPECT_EQ(cobs.lastNew, kAnotherValue);
}

TEST(SignalSlotTest, ObservableIntegralPropertyBounds) {
	// Unsigned

	ObservableIntegralProperty<uint8_t> ui8("test_uint8_t", 0);
	EXPECT_EQ(ui8.getValue(), 0);

	// Decrements
	EXPECT_THROW(ui8.decrement(), std::underflow_error);
	EXPECT_THROW(--ui8, std::underflow_error);
	ui8.setValue(5);
	EXPECT_THROW(ui8.decrement(6), std::underflow_error);

	// Increments
	ui8.setValue(1);
	EXPECT_THROW(ui8.increment(std::numeric_limits<uint8_t>::max()), std::overflow_error);
	ui8.setValue(std::numeric_limits<uint8_t>::max());
	EXPECT_THROW(++ui8, std::overflow_error);

	// Signed

	int16_t initialValue = 0;
	ObservableIntegralProperty<int16_t> i16("test_int16_t", initialValue);
	EXPECT_EQ(i16.getValue(), initialValue);

	// Increments
	EXPECT_NO_THROW(++i16);
	EXPECT_NO_THROW(i16.increment(5));
	EXPECT_EQ(i16.getValue(), initialValue + 6);
	EXPECT_THROW(i16.increment(std::numeric_limits<int16_t>::max()), std::overflow_error);
	i16.setValue(initialValue);
	EXPECT_NO_THROW(i16.increment(std::numeric_limits<int16_t>::max()));
	EXPECT_EQ(i16.getValue(), std::numeric_limits<int16_t>::max());

	// Decrements
	i16.setValue(initialValue);
	EXPECT_NO_THROW(--i16);
	EXPECT_NO_THROW(i16.decrement(5));
	EXPECT_EQ(i16.getValue(), initialValue - 6);
	// Notice that ::min here is a negative number
	EXPECT_THROW(i16.increment(std::numeric_limits<int16_t>::min()), std::underflow_error);
	i16.setValue(initialValue);
	EXPECT_NO_THROW(i16.increment(std::numeric_limits<int16_t>::min()));
	EXPECT_EQ(i16.getValue(), std::numeric_limits<int16_t>::min());
}

TEST(SignalSlotTest, ObservableIntegralPropertyTypeSize) {
	EXPECT_EQ(ObservableIntegralProperty<uint8_t>::typeSize(), sizeof(uint8_t));
	EXPECT_EQ(ObservableIntegralProperty<int32_t>::typeSize(), sizeof(int32_t));
	EXPECT_EQ(ObservableIntegralProperty<short>::typeSize(), sizeof(short));
	EXPECT_EQ(ObservableIntegralProperty<inode_t>::typeSize(), sizeof(inode_t));
}
