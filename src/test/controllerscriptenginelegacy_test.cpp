#include "controllers/scripting/legacy/controllerscriptenginelegacy.h"

#include <QElapsedTimer>
#include <QScopedPointer>
#include <QTemporaryFile>
#include <QThread>
#include <QtDebug>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <numeric>

#include "control/controlobject.h"
#include "control/controlpotmeter.h"
#include "controllers/softtakeover.h"
#include "preferences/usersettings.h"
#include "test/mixxxtest.h"
#include "util/assert.h"
#include "util/color/colorpalette.h"
#include "util/time.h"

typedef std::unique_ptr<QTemporaryFile> ScopedTemporaryFile;

const RuntimeLoggingCategory logger(QString("test").toLocal8Bit());

class ControllerScriptEngineLegacyTest : public MixxxTest {
  protected:
    static ScopedTemporaryFile makeTemporaryFile(const QString& contents) {
        QByteArray contentsBa = contents.toLocal8Bit();
        ScopedTemporaryFile pFile = std::make_unique<QTemporaryFile>();
        VERIFY_OR_DEBUG_ASSERT(pFile->open()) {
            return pFile;
        }
        pFile->write(contentsBa);
        pFile->close();
        return pFile;
    }

    void SetUp() override {
        mixxx::Time::setTestMode(true);
        mixxx::Time::setTestElapsedTime(mixxx::Duration::fromMillis(10));
        QThread::currentThread()->setObjectName("Main");
        cEngine = new ControllerScriptEngineLegacy(nullptr, logger);
        cEngine->initialize();
    }

    void TearDown() override {
        delete cEngine;
        mixxx::Time::setTestMode(false);
    }

    bool evaluateScriptFile(const QFileInfo& scriptFile) {
        return cEngine->evaluateScriptFile(scriptFile);
    }

    QJSValue evaluate(const QString& code) {
        return cEngine->jsEngine()->evaluate(code);
    }

    bool evaluateAndAssert(const QString& code) {
        return !evaluate(code).isError();
    }

    void processEvents() {
        // QCoreApplication::processEvents() only processes events that were
        // queued when the method was called. Hence, all subsequent events that
        // are emitted while processing those queued events will not be
        // processed and are enqueued for the next event processing cycle.
        // Calling processEvents() twice ensures that at least all queued and
        // the next round of emitted events are processed.
        application()->processEvents();
        application()->processEvents();
    }

    ControllerScriptEngineLegacy* cEngine;
};

class ControllerScriptEngineLegacyTimerTest : public ControllerScriptEngineLegacyTest {
  protected:
    std::unique_ptr<ControlPotmeter> co;
    std::unique_ptr<ControlPotmeter> coTimerId;

    void SetUp() override {
        ControllerScriptEngineLegacyTest::SetUp();
        co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"), -10.0, 10.0);
        co->setParameter(0.0);
        // The upper bound must be able to hold any timer ID that
        // QObject::startTimer() may return. Since Qt 6.8 these are no longer
        // small sequential numbers but encode a serial number in the upper
        // bits (e.g. 33554433), so a narrow range would silently clamp the
        // value and make the comparisons below fail.
        coTimerId = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "coTimerId"),
                -10.0,
                static_cast<double>(std::numeric_limits<int>::max()));
        coTimerId->setParameter(0.0);
        EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', 0.0);"));
        EXPECT_DOUBLE_EQ(0.0, co->get());
    }
};
TEST_F(ControllerScriptEngineLegacyTest, commonScriptHasNoErrors) {
    QFileInfo commonScript(config()->getResourcePath() +
            QStringLiteral("/controllers/common-controller-scripts.js"));
    EXPECT_TRUE(evaluateScriptFile(commonScript));
}

TEST_F(ControllerScriptEngineLegacyTest, setValue) {
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', 1.0);"));
    EXPECT_DOUBLE_EQ(1.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, getValue_InvalidKey) {
    EXPECT_TRUE(evaluateAndAssert("engine.getValue('', '');"));
    EXPECT_TRUE(evaluateAndAssert("engine.getValue('', 'invalid');"));
    EXPECT_TRUE(evaluateAndAssert("engine.getValue('[Invalid]', '');"));
}

TEST_F(ControllerScriptEngineLegacyTest, setValue_InvalidControl) {
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Nothing]', 'nothing', 1.0);"));
}

TEST_F(ControllerScriptEngineLegacyTest, getValue_InvalidControl) {
    EXPECT_TRUE(evaluateAndAssert("engine.getValue('[Nothing]', 'nothing');"));
}

TEST_F(ControllerScriptEngineLegacyTest, setValue_IgnoresNaN) {
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    co->set(10.0);
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', NaN);"));
    EXPECT_DOUBLE_EQ(10.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, getSetValue) {
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    EXPECT_TRUE(
            evaluateAndAssert("engine.setValue('[Test]', 'co', "
                              "engine.getValue('[Test]', 'co') + 1);"));
    EXPECT_DOUBLE_EQ(1.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, setParameter) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 1.0);"));
    EXPECT_DOUBLE_EQ(10.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(-10.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.5);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, setParameter_OutOfRange) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 1000);"));
    EXPECT_DOUBLE_EQ(10.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', -1000);"));
    EXPECT_DOUBLE_EQ(-10.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, setParameter_NaN) {
    // Test that NaNs are ignored.
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', NaN);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, getSetParameter) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.setParameter('[Test]', 'co', "
            "  engine.getParameter('[Test]', 'co') + 0.1);"));
    EXPECT_DOUBLE_EQ(2.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, softTakeover_setValue) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    co->setParameter(0.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.softTakeover('[Test]', 'co', true);"
            "engine.setValue('[Test]', 'co', 0.0);"));
    // The first set after enabling is always ignored.
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    // Time elapsed is not greater than the threshold, so we do not ignore this
    // set.
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', -10.0);"));
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Advance time to 2x the threshold.
    mixxx::Time::setTestElapsedTime(SoftTakeover::TestAccess::getTimeThreshold() * 2);

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    // Ignore the change since it occurred after the threshold and is too large.
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', -10.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, softTakeover_setParameter) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    co->setParameter(0.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.softTakeover('[Test]', 'co', true);"
            "engine.setParameter('[Test]', 'co', 1.0);"));
    // The first set after enabling is always ignored.
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    // Time elapsed is not greater than the threshold, so we do not ignore this
    // set.
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Advance time to 2x the threshold.
    mixxx::Time::setTestElapsedTime(SoftTakeover::TestAccess::getTimeThreshold() * 2);

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    // Ignore the change since it occurred after the threshold and is too large.
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, softTakeover_ignoreNextValue) {
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    co->setParameter(0.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.softTakeover('[Test]', 'co', true);"
            "engine.setParameter('[Test]', 'co', 1.0);"));
    // The first set after enabling is always ignored.
    EXPECT_DOUBLE_EQ(-10.0, co->get());

    // Change the control internally (putting it out of sync with the
    // ControllerEngine).
    co->setParameter(0.5);

    EXPECT_TRUE(evaluateAndAssert("engine.softTakeoverIgnoreNextValue('[Test]', 'co');"));

    // We would normally allow this set since it is below the time threshold,
    // but we are ignoring the next value.
    EXPECT_TRUE(evaluateAndAssert("engine.setParameter('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, reset) {
    // Test that NaNs are ignored.
    auto co = std::make_unique<ControlPotmeter>(ConfigKey("[Test]", "co"),
            -10.0,
            10.0);
    co->setParameter(1.0);
    EXPECT_TRUE(evaluateAndAssert("engine.reset('[Test]', 'co');"));
    EXPECT_DOUBLE_EQ(0.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTest, log) {
    EXPECT_TRUE(evaluateAndAssert("engine.log('Test that logging works.');"));
}

TEST_F(ControllerScriptEngineLegacyTest, trigger) {
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "var connection = engine.connectControl('[Test]', 'co', reaction);"
            "engine.trigger('[Test]', 'co');"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

// ControllerEngine::connectControl has a lot of quirky, inconsistent legacy behaviors
// depending on how it is invoked, so we need a lot of tests to make sure old scripts
// do not break.

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByString) {
    // Test that connecting and disconnecting by function name works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "engine.connectControl('[Test]', 'co', 'reaction');"
            "engine.trigger('[Test]', 'co');"
            "function disconnect() { "
            "  engine.connectControl('[Test]', 'co', 'reaction', 1);"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnect();"));
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByStringForbidDuplicateConnections) {
    // Test that connecting a control to a callback specified by a string
    // does not make duplicate connections. This behavior is inconsistent
    // with the behavior when specifying a callback as a function, but
    // this is how it has been done, so keep the behavior to ensure old scripts
    // do not break.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "engine.connectControl('[Test]', 'co', 'reaction');"
            "engine.connectControl('[Test]', 'co', 'reaction');"
            "engine.trigger('[Test]', 'co');"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest,
        connectControl_ByStringRedundantConnectionObjectsAreNotIndependent) {
    // Test that multiple connections are not allowed when passing
    // the callback to engine.connectControl as a function name string.
    // This is weird and inconsistent, but it is how it has been done,
    // so keep this behavior to make sure old scripts do not break.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto counter = std::make_unique<ControlObject>(ConfigKey("[Test]", "counter"));

    QString script(
            "var incrementCounterCO = function () {"
            "  let counter = engine.getValue('[Test]', 'counter');"
            "  engine.setValue('[Test]', 'counter', counter + 1);"
            "};"
            "var connection1 = engine.connectControl('[Test]', 'co', 'incrementCounterCO');"
            // Make a second connection with the same ControlObject
            // to check that disconnecting one does not disconnect both.
            "var connection2 = engine.connectControl('[Test]', 'co', 'incrementCounterCO');"
            "function changeTestCoValue() {"
            "  let testCoValue = engine.getValue('[Test]', 'co');"
            "  engine.setValue('[Test]', 'co', testCoValue + 1);"
            "};"
            "function disconnectConnection2() {"
            "  connection2.disconnect();"
            "};");

    evaluateAndAssert(script);
    EXPECT_TRUE(evaluateAndAssert(script));
    evaluateAndAssert("changeTestCoValue()");
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_EQ(1.0, counter->get());

    evaluateAndAssert("disconnectConnection2()");
    // The connection objects should refer to the same connection,
    // so disconnecting one should disconnect both.
    evaluateAndAssert("changeTestCoValue()");
    processEvents();
    EXPECT_EQ(1.0, counter->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByFunction) {
    // Test that connecting and disconnecting with a function value works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "var connection = engine.connectControl('[Test]', 'co', reaction);"
            "connection.trigger();"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByFunctionAllowDuplicateConnections) {
    // Test that duplicate connections are allowed when passing callbacks as functions.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "engine.connectControl('[Test]', 'co', reaction);"
            "engine.connectControl('[Test]', 'co', reaction);"
            // engine.trigger() has no way to know which connection to a ControlObject
            // to trigger, so it should trigger all of them.
            "engine.trigger('[Test]', 'co');"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly twice.
    EXPECT_DOUBLE_EQ(2.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_toDisconnectRemovesAllConnections) {
    // Test that every connection to a ControlObject is disconnected
    // by calling engine.connectControl(..., true). Individual connections
    // can only be disconnected by storing the connection object returned by
    // engine.connectControl and calling that object's 'disconnect' method.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "engine.connectControl('[Test]', 'co', reaction);"
            "engine.connectControl('[Test]', 'co', reaction);"
            "engine.trigger('[Test]', 'co');"
            "function disconnect() { "
            "  engine.connectControl('[Test]', 'co', reaction, 1);"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnect()"));
    processEvents();
    // The counter should have been incremented exactly twice.
    EXPECT_DOUBLE_EQ(2.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectControl_ByLambda) {
    // Test that connecting with an anonymous function works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var connection = engine.connectControl('[Test]', 'co', function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); });"
            "connection.trigger();"
            "function disconnect() { "
            "  connection.disconnect();"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnect()"));
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_Disconnect) {
    // Test that disconnecting using the 'disconnect' method on the connection
    // object returned from connectControl works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "var connection = engine.makeConnection('[Test]', 'co', reaction);"
            "connection.trigger();"
            "function disconnect() { "
            "  connection.disconnect();"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnect()"));
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_reflectDisconnect) {
    // Test that checks if disconnecting yields the appropriate feedback
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(success) { "
            "  if (success) {"
            "    let pass = engine.getValue('[Test]', 'passed');"
            "    engine.setValue('[Test]', 'passed', pass + 1.0); "
            "  }"
            "};"
            "let dummy_callback = function(value) {};"
            "let connection = engine.makeConnection('[Test]', 'co', dummy_callback);"
            "reaction(connection);"
            "reaction(connection.isConnected);"
            "let successful_disconnect = connection.disconnect();"
            "reaction(successful_disconnect);"
            "reaction(!connection.isConnected);"));
    processEvents();
    EXPECT_DOUBLE_EQ(4.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_DisconnectByPassingToConnectControl) {
    // Test that passing a connection object back to engine.connectControl
    // removes the connection
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));
    // The connections should be removed from the ControlObject which they were
    // actually connected to, regardless of the group and item arguments passed
    // to engine.connectControl() to remove the connection. All that should matter
    // is that a valid ControlObject is specified.
    auto dummy = std::make_unique<ControlObject>(ConfigKey("[Test]", "dummy"));

    EXPECT_TRUE(evaluateAndAssert(
            "var reaction = function(value) { "
            "  let pass = engine.getValue('[Test]', 'passed');"
            "  engine.setValue('[Test]', 'passed', pass + 1.0); };"
            "var connection1 = engine.connectControl('[Test]', 'co', reaction);"
            "var connection2 = engine.connectControl('[Test]', 'co', reaction);"
            "function disconnectConnection1() { "
            "  engine.connectControl('[Test]',"
            "                        'dummy',"
            "                        connection1);"
            "  engine.trigger('[Test]', 'co'); }"
            // Whether a 4th argument is passed to engine.connectControl does not matter.
            "function disconnectConnection2() { "
            "  engine.connectControl('[Test]',"
            "                        'dummy',"
            "                        connection2, true);"
            "  engine.trigger('[Test]', 'co'); }"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_TRUE(evaluateAndAssert("disconnectConnection1()"));
    processEvents();
    // The counter should have been incremented once by connection2.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
    EXPECT_TRUE(evaluateAndAssert("disconnectConnection2()"));
    processEvents();
    // The counter should not have changed.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_MakesIndependentConnection) {
    // Test that multiple connections can be made to the same CO with
    // the same callback function and that calling their 'disconnect' method
    // only disconnects the callback for that object.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto counter = std::make_unique<ControlObject>(ConfigKey("[Test]", "counter"));

    EXPECT_TRUE(evaluateAndAssert(
            "var incrementCounterCO = function () {"
            "  let counter = engine.getValue('[Test]', 'counter');"
            "  engine.setValue('[Test]', 'counter', counter + 1);"
            "};"
            "var connection1 = engine.makeConnection('[Test]', 'co', incrementCounterCO);"
            // Make a second connection with the same ControlObject
            // to check that disconnecting one does not disconnect both.
            "var connection2 = engine.makeConnection('[Test]', 'co', incrementCounterCO);"
            "function changeTestCoValue() {"
            "  let testCoValue = engine.getValue('[Test]', 'co');"
            "  engine.setValue('[Test]', 'co', testCoValue + 1);"
            "}"
            "function disconnectConnection1() {"
            "  connection1.disconnect();"
            "}"));
    EXPECT_TRUE(evaluateAndAssert("changeTestCoValue()"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    EXPECT_EQ(2.0, counter->get());

    EXPECT_TRUE(evaluateAndAssert("disconnectConnection1()"));
    // Only the callback for connection1 should have disconnected;
    // the callback for connection2 should still be connected, so
    // changing the CO they were both connected to should
    // increment the counter once.
    EXPECT_TRUE(evaluateAndAssert("changeTestCoValue()"));
    processEvents();
    EXPECT_EQ(3.0, counter->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionObject_trigger) {
    // Test that triggering using the 'trigger' method on the connection
    // object returned from connectControl works.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto counter = std::make_unique<ControlObject>(ConfigKey("[Test]", "counter"));

    EXPECT_TRUE(evaluateAndAssert(
            "var incrementCounterCO = function () {"
            "  let counter = engine.getValue('[Test]', 'counter');"
            "  engine.setValue('[Test]', 'counter', counter + 1);"
            "};"
            "var connection1 = engine.makeConnection('[Test]', 'co', incrementCounterCO);"
            // Make a second connection with the same ControlObject
            // to check that triggering a connection object only triggers that callback,
            // not every callback connected to its ControlObject.
            "var connection2 = engine.makeConnection('[Test]', 'co', incrementCounterCO);"
            "connection1.trigger();"));
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, counter->get());
}

TEST_F(ControllerScriptEngineLegacyTest, connectionExecutesWithCorrectThisObject) {
    // Test that callback functions are executed with JavaScript's
    // 'this' keyword referring to the object in which the connection
    // was created.
    auto co = std::make_unique<ControlObject>(ConfigKey("[Test]", "co"));
    auto pass = std::make_unique<ControlObject>(ConfigKey("[Test]", "passed"));

    EXPECT_TRUE(evaluateAndAssert(
            "var TestObject = function () {"
            "  this.executeTheCallback = true;"
            "  this.connection = engine.makeConnection('[Test]', 'co', function () {"
            "    if (this.executeTheCallback) {"
            "      engine.setValue('[Test]', 'passed', 1);"
            "    }"
            "  }.bind(this));"
            "};"
            "var someObject = new TestObject();"
            "someObject.connection.trigger();"));
    // ControlObjectScript connections are processed via QueuedConnection. Use
    // processEvents() to cause Qt to deliver them.
    processEvents();
    // The counter should have been incremented exactly once.
    EXPECT_DOUBLE_EQ(1.0, pass->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_repeatedTimer) {
    EXPECT_TRUE(evaluateAndAssert(
            "engine.setValue('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());

    EXPECT_TRUE(
            evaluateAndAssert(R"(engine.beginTimer(50, function() {
                                    let x = engine.getValue('[Test]', 'co');
                                    x++; 
                                    engine.setValue('[Test]', 'co', x);
                                 }, false);)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());

    cEngine->thread()->msleep(70);
    processEvents();

    EXPECT_DOUBLE_EQ(1.0, co->get());

    cEngine->thread()->msleep(140);
    processEvents();

    EXPECT_DOUBLE_EQ(2.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimer) {
    EXPECT_TRUE(evaluateAndAssert(
            "engine.setValue('[Test]', 'co', 0.0);"));
    EXPECT_DOUBLE_EQ(0.0, co->get());

    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(engine.beginTimer(20, function() {
                   engine.setValue('[Test]', 'co', 1.0);
               }, true);)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(1.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimerBindFunction) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(var globVar = 7;
            timerId = engine.beginTimer(20, function () {
                engine.setValue('[Test]', 'co', this.globVar);
                this.globVar++;
                engine.setValue('[Test]', 'coTimerId', timerId + 10);
            }.bind(this), true);            
            engine.setValue('[Test]', 'coTimerId', timerId);)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(timerId + 10, coTimerId->get());
    EXPECT_DOUBLE_EQ(7.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', this.globVar);"));
    processEvents();

    EXPECT_DOUBLE_EQ(8.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimerArrowFunction) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(var globVar = 7;
            timerId = engine.beginTimer(20, () => {
                engine.setValue('[Test]', 'co', this.globVar);
                this.globVar++;
                engine.setValue('[Test]', 'coTimerId', timerId + 10);
            }, true);            
            engine.setValue('[Test]', 'coTimerId', timerId);)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(timerId + 10, coTimerId->get());
    EXPECT_DOUBLE_EQ(7.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', this.globVar);"));
    processEvents();

    EXPECT_DOUBLE_EQ(8.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimerBindFunctionInClass) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(
            class MyClass {
               constructor() {
                  this.timerId = undefined;
                  this.globVar = 7;
               }
               runTimer() {
                  this.timerId = engine.beginTimer(20, function() {
                     engine.setValue('[Test]', 'co', this.globVar);
                     this.globVar++;
                     engine.setValue('[Test]', 'coTimerId', this.timerId + 10);
                  }.bind(this), true);            
                  engine.setValue('[Test]', 'coTimerId', this.timerId);
               }
            }
            var MyMapping = new MyClass();
            MyMapping.runTimer();)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(timerId + 10, coTimerId->get());
    EXPECT_DOUBLE_EQ(7.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', MyMapping.globVar);"));
    processEvents();

    EXPECT_DOUBLE_EQ(8.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_singleShotTimerArrowFunctionInClass) {
    EXPECT_TRUE(evaluateAndAssert(
            R"(
            class MyClass {
               constructor() {
                  this.timerId = undefined;
                  this.globVar = 7;
               }
               runTimer() {                  
                  const savedThis = this;
                  this.timerId = engine.beginTimer(20, () => {
                     if (savedThis !== this) { throw new Error("savedThis should be equal to this"); }
                     if (!(this instanceof MyClass)) { throw new Error("this should be an instance of MyClass"); }
                     engine.setValue('[Test]', 'co', this.globVar);
                     this.globVar++;
                     engine.setValue('[Test]', 'coTimerId', this.timerId + 10);
                  }, true);            
                  engine.setValue('[Test]', 'coTimerId', this.timerId);
               }
            }
            var MyMapping = new MyClass();
            MyMapping.runTimer();)"));
    processEvents();
    EXPECT_DOUBLE_EQ(0.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(timerId + 10, coTimerId->get());
    EXPECT_DOUBLE_EQ(7.0, co->get());
    EXPECT_TRUE(evaluateAndAssert("engine.setValue('[Test]', 'co', MyMapping.globVar);"));
    processEvents();

    EXPECT_DOUBLE_EQ(8.0, co->get());
}

TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_repeatedTimerArrowFunctionCallInClass) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(
            class MyClass {
               constructor() {
                  this.timerId = undefined;
                  this.globVar = 7;
               }
               stopTimer() {
                  if (!(this instanceof MyClass)) { throw new Error("this should be an instance of MyClass"); }
                  engine.stopTimer(this.timerId);
                  this.timerId = 0;
                  engine.setValue('[Test]', 'coTimerId', this.timerId + 20);
               }
               runTimer() {
                  this.timerId = engine.beginTimer(20, () => this.stopTimer(), false);                  
                  engine.setValue('[Test]', 'co', this.globVar);      
                  engine.setValue('[Test]', 'coTimerId', this.timerId);
               }
            }
            var MyMapping = new MyClass();
            MyMapping.runTimer();)"));
    processEvents();
    EXPECT_DOUBLE_EQ(7.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(20, coTimerId->get());

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(20, coTimerId->get());
}
TEST_F(ControllerScriptEngineLegacyTimerTest, beginTimer_repeatedTimerThisFunctionCallInClass) {
    // Single shot timer with minimum allowed interval of 20ms
    EXPECT_TRUE(evaluateAndAssert(
            R"(
            class MyClass {
               constructor() {
                  this.timerId = undefined;
                  this.globVar = 7;
               }
               stopTimer() {    
                  if (!(this instanceof MyClass)) {throw new Error("this should be an instance of MyClass");}
                  engine.stopTimer(this.timerId);
                  this.timerId = 0;
                  engine.setValue('[Test]', 'coTimerId', this.timerId + 20);
               }
               runTimer() {
                  this.timerId = engine.beginTimer(20, this.stopTimer.bind(this), false);              
                  engine.setValue('[Test]', 'co', this.globVar);
                  engine.setValue('[Test]', 'coTimerId', this.timerId);
               }
            }
            var MyMapping = new MyClass();
            MyMapping.runTimer();)"));
    processEvents();
    EXPECT_DOUBLE_EQ(7.0, co->get());
    double timerId = coTimerId->get();
    EXPECT_TRUE(timerId > 0);

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(20, coTimerId->get());

    cEngine->thread()->msleep(35);
    processEvents();

    EXPECT_DOUBLE_EQ(20, coTimerId->get());
}

// Bite DJ vinyl brake: releasing a thrown jog wheel coasts the platter to its
// target rate at the deceleration [BiteDJ],vinyl_brake configures, instead of
// stock Mixxx's alpha-beta ramp which pulls it there in a fraction of a second.
class ControllerScriptEngineLegacyVinylBrakeTest
        : public ControllerScriptEngineLegacyTest {
  protected:
    void SetUp() override {
        ControllerScriptEngineLegacyTest::SetUp();
        m_pBrake = makeCo("[BiteDJ]", "vinyl_brake");
        m_pScratch2 = makeCo(kGroup, "scratch2");
        m_pScratch2Enable = makeCo(kGroup, "scratch2_enable");
        m_pPlay = makeCo(kGroup, "play");
        m_pTrackLoaded = makeCo(kGroup, "track_loaded");
        m_pRateRatio = makeCo(kGroup, "rate_ratio");
        m_pReverse = makeCo(kGroup, "reverse");
        m_pTrackLoaded->set(1.0);
    }

    static std::unique_ptr<ControlObject> makeCo(
            const QString& group, const QString& item) {
        return std::make_unique<ControlObject>(ConfigKey(group, item));
    }

    // Grabs the platter and lets go of it again at `rate`, without letting the
    // scratch timer run in between - so the release sees exactly this rate.
    // The deck is already scratching when the grab happens, which is what makes
    // scratchEnable adopt `rate` as the filter's initial velocity too: mid-throw
    // the wheel is moving, and both the stock ramp and the brake have to start
    // from there for the comparison between them to mean anything.
    void throwAndRelease(double rate) {
        m_pScratch2Enable->set(1.0);
        m_pScratch2->set(rate);
        EXPECT_TRUE(evaluateAndAssert(
                "engine.scratchEnable(1, 128, 33+1/3, 1/8, 1/32);"));
        m_pScratch2->set(rate);
        EXPECT_TRUE(evaluateAndAssert("engine.scratchDisable(1);"));
    }

    // One observation of a coasting platter: the scratch rate, and how long
    // after the release it was seen. The coast is driven by the clock rather
    // than by a tick count, so time - not tick index - is what the assertions
    // below are allowed to reason about.
    struct CoastSample {
        double seconds;
        double rate;
    };

    // Runs the scratch timer to a standstill, returning the rates seen along
    // the way, starting with the rate at the moment of release.
    QVector<CoastSample> coast(int maxPasses = 4000) {
        QElapsedTimer since;
        since.start();
        QVector<CoastSample> samples{CoastSample{0.0, m_pScratch2->get()}};
        for (int i = 0; i < maxPasses && m_pScratch2Enable->get() != 0.0; ++i) {
            cEngine->thread()->msleep(1);
            processEvents();
            const double now = m_pScratch2->get();
            if (now != samples.constLast().rate) {
                samples.append(CoastSample{since.nsecsElapsed() / 1e9, now});
            }
        }
        return samples;
    }

    // How long a coast should take, from `distance` away from its target, at a
    // given [BiteDJ],vinyl_brake time. Mirrors startVinylBrake(): solving
    // d' = -(k1*d + k2) with k2 = k1/ratio gives ln(1 + ratio*d) / k1, and k1
    // is scaled so a 1x throw takes exactly the configured time.
    static double coastSeconds(double brakeSeconds, double distance) {
        constexpr double kRatio = 3.0;
        return brakeSeconds * std::log(1.0 + kRatio * distance) /
                std::log(1.0 + kRatio);
    }

    // Checks the platter closed on `target` and got there: every sample is
    // nearer than the one before, none overshoots, and the last one is the
    // target itself. Arriving matters - drag proportional to speed alone only
    // ever approaches its target, which leaves the end of the run-out an
    // inaudible crawl, and that is what makes a brake read as a snap-back.
    static void expectClosesOn(double target, const QVector<CoastSample>& samples) {
        ASSERT_GT(samples.size(), 3);
        for (int i = 1; i < samples.size(); ++i) {
            const double before = samples[i - 1].rate - target;
            const double after = samples[i].rate - target;
            EXPECT_LT(std::fabs(after), std::fabs(before)) << "sample " << i;
            EXPECT_GE(before * after, 0.0) << "sample " << i << " overshot";
        }
        EXPECT_DOUBLE_EQ(target, samples.constLast().rate);
    }

    // Checks the run-out lasted as long as the setting promises. The coast
    // steps by elapsed time, so this holds however coarsely the 1ms scratch
    // timer actually fires - it can only run over by the single tick that
    // carries the platter across the line.
    static void expectLasts(double expectedSeconds, const QVector<CoastSample>& samples) {
        const double actual = samples.constLast().seconds;
        EXPECT_GT(actual, expectedSeconds * 0.75) << "run-out cut short";
        EXPECT_LT(actual, expectedSeconds * 1.5 + 0.05) << "run-out overran";
    }

    static constexpr const char* kGroup = "[Channel1]";

    std::unique_ptr<ControlObject> m_pBrake;
    std::unique_ptr<ControlObject> m_pScratch2;
    std::unique_ptr<ControlObject> m_pScratch2Enable;
    std::unique_ptr<ControlObject> m_pPlay;
    std::unique_ptr<ControlObject> m_pTrackLoaded;
    std::unique_ptr<ControlObject> m_pRateRatio;
    std::unique_ptr<ControlObject> m_pReverse;
};

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, coastsToAStandstill) {
    // 0.2s for a 1x throw to run out; a -2.0 backspin takes a little longer,
    // because it has further to fall.
    m_pBrake->set(0.2);
    throwAndRelease(-2.0);

    // The release must not have touched the rate: the whole point is that the
    // platter is still moving when the DJ's hand comes off it.
    EXPECT_DOUBLE_EQ(-2.0, m_pScratch2->get());
    EXPECT_DOUBLE_EQ(1.0, m_pScratch2Enable->get());

    const QVector<CoastSample> samples = coast();
    expectClosesOn(0.0, samples);
    expectLasts(coastSeconds(0.2, 2.0), samples);

    EXPECT_DOUBLE_EQ(0.0, m_pScratch2->get());
    EXPECT_DOUBLE_EQ(0.0, m_pScratch2Enable->get());
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, harderThrowsShedSpeedHarder) {
    // Part of the drag grows with speed, so a wheel thrown four times as hard
    // is also slowing harder at the moment it is released, and how hard the DJ
    // spun it stays audible.
    m_pBrake->set(0.2);

    throwAndRelease(-1.0);
    const QVector<CoastSample> soft = coast();
    expectClosesOn(0.0, soft);

    throwAndRelease(-4.0);
    const QVector<CoastSample> hard = coast();
    expectClosesOn(0.0, hard);

    // Four times the throw is nowhere near four times the run-out: the extra
    // speed is mostly spent on the extra drag it brings with it.
    EXPECT_LT(hard.constLast().seconds, soft.constLast().seconds * 2.5);
    // But it is still longer - a bigger throw does last longer.
    EXPECT_GT(hard.constLast().seconds, soft.constLast().seconds);
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, keepsClosingToTheEnd) {
    // The regression this guards: with drag proportional to speed and nothing
    // else, a run-out front-loads so hard that it is perceptually over long
    // before the setting says, and the platter only crawls the rest of the way.
    // Constant friction alongside the drag keeps the last half of the coast
    // carrying a real share of the distance. A pure decay would be down to
    // ~22% of the throw at the half-way mark, and would never arrive at all.
    m_pBrake->set(0.3);
    throwAndRelease(-1.0);

    const QVector<CoastSample> samples = coast();
    expectClosesOn(0.0, samples);

    const double total = samples.constLast().seconds;
    double halfwayRate = 0.0;
    for (const CoastSample& sample : samples) {
        if (sample.seconds <= total / 2.0) {
            halfwayRate = sample.rate;
        }
    }
    EXPECT_GT(std::fabs(halfwayRate), 0.25)
            << "the second half of the run-out has nothing left to say";
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, coastsToThePlayRateOnAPlayingDeck) {
    m_pBrake->set(0.2);
    m_pPlay->set(1.0);
    m_pRateRatio->set(1.0);
    throwAndRelease(-2.0);

    const QVector<CoastSample> samples = coast();
    expectClosesOn(1.0, samples);
    // The distance that matters is to the play rate, not to a standstill.
    expectLasts(coastSeconds(0.2, 3.0), samples);
    // A playing deck is picked back up by its own rate, not left stopped.
    EXPECT_DOUBLE_EQ(1.0, m_pScratch2->get());
    EXPECT_DOUBLE_EQ(0.0, m_pScratch2Enable->get());
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, brakeOffKeepsTheStockRamp) {
    m_pBrake->set(0.0);
    throwAndRelease(-2.0);

    const QVector<CoastSample> samples = coast();
    ASSERT_GT(samples.size(), 2);
    // The stock ramp converges on its target and gives up once it is within
    // 0.00001 of it, where the brake instead lands exactly on the target.
    // Ending near zero but not on it is what says the brake stayed out of this
    // release.
    EXPECT_NE(0.0, samples.constLast().rate);
    EXPECT_LT(std::fabs(samples.constLast().rate), 0.0001);

    EXPECT_DOUBLE_EQ(0.0, m_pScratch2Enable->get());
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, aNudgeIsNotWorthCoasting) {
    m_pBrake->set(0.1);
    // Released within kVinylBrakeMinRate of where it was headed anyway, so the
    // brake stays out of it and the stock ramp finishes the move.
    throwAndRelease(-0.02);

    const QVector<CoastSample> samples = coast();
    // Had the brake taken this release it would have found it already inside
    // the cutoff and snapped it onto the target in one tick; the stock ramp
    // instead closes the gap over many.
    EXPECT_GT(samples.size(), 3);
    EXPECT_DOUBLE_EQ(0.0, m_pScratch2Enable->get());
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, aDeckStartedMidCoastIsPickedUp) {
    // Let go of the platter with the deck stopped, so the run-out is headed for
    // a standstill, then start the deck part way through it. The brake must hand
    // the track its own speed back rather than carry on dragging it down to
    // nothing.
    //
    // Note a *press* of play or pause never gets this far in the real app -
    // EngineBuffer::slotControlPlayRequest takes the deck off the scratch
    // outright, the way a cue press does (aCuePressEndsTheCoast). What is under
    // test here is that the coast re-reads where it is headed on every tick
    // rather than latching it at the release, which is also what carries it to
    // the right place when the tempo fader is moved mid-run-out.
    m_pBrake->set(2.0);
    m_pRateRatio->set(1.0);
    throwAndRelease(-2.0);

    for (int i = 0; i < 20; ++i) {
        cEngine->thread()->msleep(1);
        processEvents();
    }
    const double midCoast = m_pScratch2->get();
    ASSERT_LT(midCoast, -1.0) << "coast ended far too early";

    m_pPlay->set(1.0);
    const QVector<CoastSample> samples = coast();
    expectClosesOn(1.0, samples);
    EXPECT_DOUBLE_EQ(1.0, m_pScratch2->get());
    EXPECT_DOUBLE_EQ(0.0, m_pScratch2Enable->get());
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, aStoppedDeckMidCoastSpinsDown) {
    // The mirror image: a deck taken off play mid-run-out stops chasing a rate
    // nothing is playing at. Same caveat as above - a pause *press* ends the
    // coast rather than re-aiming it.
    m_pBrake->set(2.0);
    m_pPlay->set(1.0);
    m_pRateRatio->set(1.0);
    throwAndRelease(-2.0);

    for (int i = 0; i < 20; ++i) {
        cEngine->thread()->msleep(1);
        processEvents();
    }
    ASSERT_LT(m_pScratch2->get(), -1.0) << "coast ended far too early";

    m_pPlay->set(0.0);
    const QVector<CoastSample> samples = coast();
    expectClosesOn(0.0, samples);
    EXPECT_DOUBLE_EQ(0.0, m_pScratch2Enable->get());
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, aCuePressEndsTheCoast) {
    // CueControl::endScratching clears scratch2_enable so a cue plays from its
    // own position at the track's speed however much run-out is left. The coast
    // has to notice it no longer owns the deck: keep writing scratch2 and the
    // next grab of the platter would pick a stale rate back up, and the 1ms
    // timer would stay up for the rest of the brake time for nothing.
    m_pBrake->set(2.0);
    throwAndRelease(-2.0);

    for (int i = 0; i < 20; ++i) {
        cEngine->thread()->msleep(1);
        processEvents();
    }
    ASSERT_LT(m_pScratch2->get(), -1.0) << "coast ended far too early";

    m_pScratch2->set(0.0);
    m_pScratch2Enable->set(0.0);
    for (int i = 0; i < 20; ++i) {
        cEngine->thread()->msleep(1);
        processEvents();
    }
    EXPECT_DOUBLE_EQ(0.0, m_pScratch2->get()) << "the coast kept driving the deck";
    EXPECT_DOUBLE_EQ(0.0, m_pScratch2Enable->get());
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, aHeldPlatterTakesItselfBack) {
    // Same clearing, but with the wheel still under the DJ's hand: cueing while
    // scratching must not leave the platter dead until it is let go of and
    // grabbed again, so the scratch timer takes the deck straight back.
    m_pBrake->set(2.0);
    m_pScratch2Enable->set(1.0);
    EXPECT_TRUE(evaluateAndAssert(
            "engine.scratchEnable(1, 128, 33+1/3, 1/8, 1/32);"));

    m_pScratch2Enable->set(0.0);
    cEngine->thread()->msleep(5);
    processEvents();

    EXPECT_DOUBLE_EQ(1.0, m_pScratch2Enable->get());

    // And a real release still gives it up.
    EXPECT_TRUE(evaluateAndAssert("engine.scratchDisable(1, false);"));
    for (int i = 0; i < 20; ++i) {
        cEngine->thread()->msleep(1);
        processEvents();
    }
    EXPECT_DOUBLE_EQ(0.0, m_pScratch2Enable->get());
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, grabbingThePlatterEndsTheCoast) {
    // Long enough that 20ms of coasting cannot have taken the throw far.
    m_pBrake->set(2.0);
    throwAndRelease(-2.0);

    for (int i = 0; i < 20; ++i) {
        cEngine->thread()->msleep(1);
        processEvents();
    }
    const double midCoast = m_pScratch2->get();
    EXPECT_LT(midCoast, -1.0) << "coast ended far too early";

    // Hand back on the wheel: scratching resumes from the coasting rate rather
    // than from a standstill, and the brake no longer drives the deck.
    EXPECT_TRUE(evaluateAndAssert(
            "engine.scratchEnable(1, 128, 33+1/3, 1/8, 1/32);"));
    EXPECT_DOUBLE_EQ(1.0, m_pScratch2Enable->get());

    for (int i = 0; i < 20; ++i) {
        cEngine->thread()->msleep(1);
        processEvents();
    }
    // A held, motionless wheel is dragged towards 0 through the filter, which
    // is far more abrupt than the two seconds of brake it was handed to.
    EXPECT_GT(m_pScratch2->get(), midCoast + 0.5);
}

TEST_F(ControllerScriptEngineLegacyVinylBrakeTest, softStartStartsTheDeckFirst) {
    // Starting a deck takes it off whatever scratch is driving it
    // (EngineBuffer::slotControlPlayRequest), so a soft start has to write play
    // before it enables scratch2 - the stock order pulls the platter out from
    // under itself, and scratchProcess then finds the flag cleared on its next
    // tick and abandons the ramp.
    m_pRateRatio->set(1.0);

    double enableWhenStarted = -1.0;
    QObject::connect(m_pPlay.get(),
            &ControlObject::valueChanged,
            m_pPlay.get(),
            [this, &enableWhenStarted](double value) {
                if (value != 0.0 && enableWhenStarted < 0.0) {
                    enableWhenStarted = m_pScratch2Enable->get();
                }
            });

    EXPECT_TRUE(evaluateAndAssert("engine.softStart(1, true);"));
    EXPECT_DOUBLE_EQ(1.0, m_pPlay->get());
    EXPECT_DOUBLE_EQ(0.0, enableWhenStarted)
            << "the scratch was taken before the deck was started";
    EXPECT_DOUBLE_EQ(1.0, m_pScratch2Enable->get());

    // And the ramp is still running a good while later, climbing from the
    // standstill towards the deck's rate. The regression this guards is a ramp
    // abandoned on its first tick, which leaves the platter at a standstill with
    // the scratch given up - a soft start that never starts.
    for (int i = 0; i < 50; ++i) {
        cEngine->thread()->msleep(1);
        processEvents();
    }
    EXPECT_DOUBLE_EQ(1.0, m_pScratch2Enable->get()) << "the soft start was abandoned";
    EXPECT_GT(m_pScratch2->get(), 0.0);
    EXPECT_LT(m_pScratch2->get(), 1.0) << "a soft start is a ramp, not a jump";
}
