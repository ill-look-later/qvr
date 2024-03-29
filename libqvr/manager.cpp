/*
 * Copyright (C) 2016 Computer Graphics Group, University of Siegen
 * Written by Martin Lambers <martin.lambers@uni-siegen.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <QDir>
#include <QQueue>
#include <QDesktopWidget>
#include <QApplication>
#include <QTimer>

#include "manager.hpp"
#include "event.hpp"
#include "logging.hpp"
#include "config.hpp"
#include "app.hpp"
#include "observer.hpp"
#include "window.hpp"
#include "process.hpp"

QVRManager* manager = NULL; // singleton instance
QQueue<QVREvent>* eventQueue = NULL; // single event queue for the singleton

static bool parseLogLevel(const QString& ll, QVRLogLevel* logLevel)
{
    if (ll.compare("fatal", Qt::CaseInsensitive) == 0)
        *logLevel = QVR_Log_Level_Fatal;
    else if (ll.compare("warning", Qt::CaseInsensitive) == 0)
        *logLevel = QVR_Log_Level_Warning;
    else if (ll.compare("info", Qt::CaseInsensitive) == 0)
        *logLevel = QVR_Log_Level_Info;
    else if (ll.compare("debug", Qt::CaseInsensitive) == 0)
        *logLevel = QVR_Log_Level_Debug;
    else if (ll.compare("firehose", Qt::CaseInsensitive) == 0)
        *logLevel = QVR_Log_Level_Firehose;
    else {
        // fall back to default
        *logLevel = QVR_Log_Level_Info;
        return false;
    }
    return true;
}

static void removeTwoArgs(int& argc, char* argv[], int i)
{
    for (int j = i; j < argc - 2; j++)
        argv[j] = argv[j + 2];
    argc -= 2;
}

static void removeArg(int& argc, char* argv[], int i)
{
    for (int j = i; j < argc - 1; j++)
        argv[j] = argv[j + 1];
    argc -= 1;
}

QVRManager::QVRManager(int& argc, char* argv[]) :
    _triggerTimer(new QTimer),
    _fpsTimer(new QTimer),
    _logLevel(QVR_Log_Level_Warning),
    _workingDir(),
    _processIndex(0),
    _syncToVblank(true),
    _fpsMsecs(0),
    _fpsCounter(0),
    _configFilename(),
    _app(NULL),
    _config(NULL),
    _observers(),
    _customObservers(),
    _masterWindow(NULL),
    _masterGLContext(NULL),
    _windows(),
    _thisProcess(NULL),
    _slaveProcesses(),
    _wantExit(false)
{
    Q_ASSERT(!manager);  // there can be only one
    manager = this;
    eventQueue = new QQueue<QVREvent>;
    Q_INIT_RESOURCE(qvr);

    // set log level
    if (::getenv("QVR_LOG_LEVEL"))
        parseLogLevel(::getenv("QVR_LOG_LEVEL"), &_logLevel);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--qvr-log-level") == 0 && i < argc - 1) {
            parseLogLevel(argv[i + 1], &_logLevel);
            removeTwoArgs(argc, argv, i);
            break;
        } else if (strncmp(argv[i], "--qvr-log-level=", 16) == 0) {
            parseLogLevel(argv[i] + 16, &_logLevel);
            removeArg(argc, argv, i);
            break;
        }
    }

    // set working directory
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--qvr-wd=", 9) == 0) {
            _workingDir = argv[i] + 9;
            removeArg(argc, argv, i);
            break;
        }
    }

    // get process index (it is 0 == master by default)
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--qvr-process=", 14) == 0) {
            _processIndex = ::atoi(argv[i] + 14);
            removeArg(argc, argv, i);
            break;
        }
    }

    // set sync-to-vblank
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--qvr-sync-to-vblank") == 0 && i < argc - 1) {
            _syncToVblank = ::atoi(argv[i + 1]);
            removeTwoArgs(argc, argv, i);
            break;
        } else if (strncmp(argv[i], "--qvr-sync-to-vblank=", 21) == 0) {
            _syncToVblank = ::atoi(argv[i] + 21);
            removeArg(argc, argv, i);
            break;
        }
    }

    // set FPS printing (only on master)
    if (_processIndex == 0) {
        if (::getenv("QVR_FPS"))
            _fpsMsecs = ::atoi(::getenv("QVR_FPS"));
        for (int i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--qvr-fps") == 0 && i < argc - 1) {
                _fpsMsecs = ::atoi(argv[i + 1]);
                removeTwoArgs(argc, argv, i);
                break;
            } else if (strncmp(argv[i], "--qvr-fps=", 10) == 0) {
                _fpsMsecs = ::atoi(argv[i] + 10);
                removeArg(argc, argv, i);
                break;
            }
        }
    }

    // get configuration file name (if any)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--qvr-config") == 0 && i < argc - 1) {
            _configFilename = argv[i + 1];
            removeTwoArgs(argc, argv, i);
            break;
        } else if (strncmp(argv[i], "--qvr-config=", 13) == 0) {
            _configFilename = argv[i] + 13;
            removeArg(argc, argv, i);
            break;
        }
    }

    // preserve remaining command line content for slave processes
    for (int i = 1; i < argc; i++)
        _appArgs << argv[i];
}

QVRManager::~QVRManager()
{
    for (int i = 0; i < _observers.size(); i++)
        delete _observers.at(i);
    for (int i = 0; i < _windows.size(); i++)
        delete _windows.at(i);
    for (int i = 0; i < _slaveProcesses.size(); i++)
        delete _slaveProcesses.at(i);
    delete _masterWindow;
    delete _thisProcess;
    delete _config;
    delete _triggerTimer;
    delete _fpsTimer;
    delete eventQueue;
    eventQueue = NULL;
    manager = NULL;
}

bool QVRManager::init(QVRApp* app)
{
    Q_ASSERT(!_app);

    _app = app;

    if (!_workingDir.isEmpty())
        QDir::setCurrent(_workingDir);

    _config = new QVRConfig;
    if (_configFilename.isEmpty()) {
        _config->createDefault();
    } else {
        if (!_config->readFromFile(_configFilename)) {
            return false;
        }
    }

    // Create observers
    _haveWasdqeObservers = false;
    _haveVrpnObservers = false;
    for (int o = 0; o < _config->observerConfigs().size(); o++) {
        _observers.append(new QVRObserver(o));
        if (_config->observerConfigs()[o].navigationType() == QVR_Navigation_Custom
                || _config->observerConfigs()[o].trackingType() == QVR_Tracking_Custom)
            _customObservers.append(_observers[o]);
        if (_config->observerConfigs()[o].navigationType() == QVR_Navigation_WASDQE)
            _haveWasdqeObservers = true;
        if (_config->observerConfigs()[o].navigationType() == QVR_Navigation_VRPN
                || _config->observerConfigs()[o].trackingType() == QVR_Tracking_VRPN)
            _haveVrpnObservers = true;
    }
    if (_haveWasdqeObservers) {
        for (int i = 0; i < 6; i++)
            _wasdqeIsPressed[i] = false;
        _wasdqeMouseProcessIndex = -1;
        _wasdqeMouseWindowIndex = -1;
        _wasdqeMouseInitialized = false;
        _wasdqePos = QVector3D(0.0f, 0.0f, 0.0f);
        _wasdqeHorzAngle = 0.0f;
        _wasdqeVertAngle = 0.0f;
    }
    if (_haveVrpnObservers) {
#ifdef HAVE_VRPN
#else
        QVR_FATAL("VRPN observers configured but VRPN is not available");
        return false;
#endif
    }

    // Create processes
    _thisProcess = new QVRProcess(_processIndex);
    if (_processIndex == 0) {
        if (_config->processConfigs().size() > 1) {
            QByteArray serializedStatData;
            QDataStream serializationDataStream(&serializedStatData, QIODevice::WriteOnly);
            _app->serializeStaticData(serializationDataStream);
            for (int p = 1; p < _config->processConfigs().size(); p++) {
                QVRProcess* process = new QVRProcess(p);
                _slaveProcesses.append(process);
                QVR_INFO("launching slave process %s (index %d) ...", qPrintable(process->id()), p);
                if (!process->launch(_configFilename, _logLevel, p, _syncToVblank, _appArgs))
                    return false;
                QVR_INFO("... initializing with %d bytes of static application data ...", serializedStatData.size());
                process->sendCmdInit(serializedStatData);
                process->flush();
                QVR_INFO("... done");
            }
        }
    } else {
        char cmd = '\0';
        _thisProcess->receiveCmd(&cmd);
        Q_ASSERT(cmd == 'i');
        QVR_INFO("initializing slave process %s (index %d) ...", qPrintable(_thisProcess->id()), _processIndex);
        _thisProcess->receiveCmdInit(_app);
        QVR_INFO("... done");
    }

    // Find out about our desktop and available screens
    QDesktopWidget* desktop = QApplication::desktop();
    QVR_INFO("process %s (index %d) uses %s which has %d screens",
            qPrintable(processConfig().id()), _processIndex,
            processConfig().display().isEmpty()
            ? "default display"
            : qPrintable(QString("display %1").arg(processConfig().display())),
            desktop->screenCount());
    for (int i = 0; i < desktop->screenCount(); i++) {
        QRect r = desktop->screenGeometry(i);
        QVR_INFO("  screen %d geometry: %d %d %dx%d", i, r.x(), r.y(), r.width(), r.height());
    }

    // Create windows
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setSwapInterval(_syncToVblank ? 1 : 0);
    QSurfaceFormat::setDefaultFormat(format);
    QVR_INFO("process %s (index %d) creating %d windows",
            qPrintable(processConfig().id()), _processIndex,
            processConfig().windowConfigs().size());
    if (processConfig().windowConfigs().size() > 0) {
        for (int w = 0; w < processConfig().windowConfigs().size(); w++) {
            int ds = windowConfig(_processIndex, w).initialDisplayScreen();
            if (ds < -1 || ds >= desktop->screenCount()) {
                QVR_FATAL("display has no screen %d", ds);
                return false;
            }
        }
        _masterGLContext = new QOpenGLContext();
        QVR_INFO("  master window...");
        _masterWindow = new QVRWindow(_masterGLContext, 0, -1, -1);
        if (!_masterWindow->isValid())
            return false;
        for (int w = 0; w < processConfig().windowConfigs().size(); w++) {
            QVR_INFO("  window %d...", w);
            QVRObserver* observer = _observers.at(windowConfig(_processIndex, w).observerIndex());
            QVRWindow* window = new QVRWindow(_masterGLContext, observer, _processIndex, w);
            if (!window->isValid())
                return false;
            _windows.append(window);
        }
    }

    // Initialize application process and windows
    if (_masterWindow)
        _masterWindow->winContext()->makeCurrent(_masterWindow);
    if (!_app->initProcess(_thisProcess))
        return false;
    for (int w = 0; w < _windows.size(); w++)
        if (!_app->initWindow(_windows[w]))
            return false;
    if (_masterWindow)
        _masterWindow->winContext()->doneCurrent();
    if (_processIndex == 0) {
        _app->update();
        _app->updateObservers(_customObservers);
    }

    // Initialize FPS printing
    if (_fpsMsecs > 0) {
        connect(_fpsTimer, SIGNAL(timeout()), this, SLOT(printFps()));
        _fpsTimer->start(_fpsMsecs);
    }

    // Initialize render loop (only on master process)
    if (_processIndex == 0) {
        // Set up timer to trigger master loop
        QObject::connect(_triggerTimer, SIGNAL(timeout()), this, SLOT(masterLoop()));
        _triggerTimer->start();
    } else {
        // Set up timer to trigger slave loop
        QObject::connect(_triggerTimer, SIGNAL(timeout()), this, SLOT(slaveLoop()));
        _triggerTimer->start();
    }

    QApplication::processEvents();

    return true;
}

void QVRManager::masterLoop()
{
    Q_ASSERT(_processIndex == 0);

    QApplication::processEvents();
    QVR_FIREHOSE("masterLoop() ...");

    if (_masterWindow)
        _masterWindow->winContext()->makeCurrent(_masterWindow);

    if (_wantExit || _app->wantExit()) {
        QVR_FIREHOSE("  ... exit now!");
        _triggerTimer->stop();
        for (int p = _slaveProcesses.size() - 1; p >= 0; p--) {
            _slaveProcesses[p]->sendCmdQuit();
            _slaveProcesses[p]->flush();
            _slaveProcesses[p]->exit();
        }
        quit();
        return;
    }

    for (int o = 0; o < _observers.size(); o++) {
        QVRObserver* obs = _observers[o];
        QVR_FIREHOSE("  ... updating observer %d", o);
        if (obs->config().navigationType() == QVR_Navigation_WASDQE) {
            const float speed = 0.04f; // TODO: make this configurable?
            if (_wasdqeIsPressed[0] || _wasdqeIsPressed[1] || _wasdqeIsPressed[2] || _wasdqeIsPressed[3]) {
                QQuaternion viewerRot = obs->trackingOrientation() * obs->navigationOrientation();
                QVector3D dir;
                if (_wasdqeIsPressed[0])
                    dir = viewerRot * QVector3D(0.0f, 0.0f, -1.0f);
                else if (_wasdqeIsPressed[1])
                    dir = viewerRot * QVector3D(-1.0f, 0.0f, 0.0f);
                else if (_wasdqeIsPressed[2])
                    dir = viewerRot * QVector3D(0.0f, 0.0f, +1.0f);
                else if (_wasdqeIsPressed[3])
                    dir = viewerRot * QVector3D(+1.0f, 0.0f, 0.0f);
                dir.setY(0.0f);
                dir.normalize();
                _wasdqePos += speed * dir;
            }
            if (_wasdqeIsPressed[4]) {
                _wasdqePos += speed * QVector3D(0.0f, +1.0f, 0.0f);
            }
            if (_wasdqeIsPressed[5]) {
                _wasdqePos += speed * QVector3D(0.0f, -1.0f, 0.0f);
            }
            obs->setNavigation(_wasdqePos + obs->config().initialNavigationPosition(),
                    QQuaternion::fromEulerAngles(_wasdqeVertAngle, _wasdqeHorzAngle, 0.0f)
                    * obs->config().initialNavigationOrientation());
        }
        if (obs->config().trackingType() == QVR_Tracking_Oculus) {
            for (int w = 0; w < _windows.size(); w++) {
                if (_windows[w]->observerId() == obs->id())
                    _windows[w]->updateObserver();
            }
        }
        if (obs->config().navigationType() == QVR_Navigation_VRPN
                || obs->config().trackingType() == QVR_Tracking_VRPN) {
            obs->update();
        }
    }

    _app->getNearFar(_near, _far);

    if (_slaveProcesses.size() > 0) {
        if (_haveWasdqeObservers) {
            QByteArray serializedWasdqeState;
            QDataStream serializationDataStream(&serializedWasdqeState, QIODevice::WriteOnly);
            serializationDataStream << _wasdqeMouseProcessIndex << _wasdqeMouseWindowIndex << _wasdqeMouseInitialized;
            QVR_FIREHOSE("  ... sending wasdqe state to slave processes");
            for (int p = 0; p < _slaveProcesses.size(); p++)
                _slaveProcesses[p]->sendCmdWasdqeState(serializedWasdqeState);
        }
        for (int o = 0; o < _observers.size(); o++) {
            QVRObserver* obs = _observers[o];
            if (obs->config().navigationType() != QVR_Navigation_Stationary
                    || obs->config().trackingType() != QVR_Tracking_Stationary) {
                QByteArray serializedObserver;
                QDataStream serializationDataStream(&serializedObserver, QIODevice::WriteOnly);
                serializationDataStream << (*_observers[o]);
                QVR_FIREHOSE("  ... sending observer %d (%d bytes) to slave processes", o, serializedObserver.size());
                for (int p = 0; p < _slaveProcesses.size(); p++)
                    _slaveProcesses[p]->sendCmdObserver(serializedObserver);
            }
        }
        QByteArray serializedDynData;
        QDataStream serializationDataStream(&serializedDynData, QIODevice::WriteOnly);
        _app->serializeDynamicData(serializationDataStream);
        QVR_FIREHOSE("  ... sending dynamic application data (%d bytes) to slave processes", serializedDynData.size());
        for (int p = 0; p < _slaveProcesses.size(); p++) {
            _slaveProcesses[p]->sendCmdRender(_near, _far, serializedDynData);
            _slaveProcesses[p]->flush();
        }
    }

    render();

    // process events and run application updates while the windows wait for the buffer swap
    QVR_FIREHOSE("  ... app update");
    processEventQueue();
    _app->update();
    _app->updateObservers(_customObservers);

    // now wait for windows to finish buffer swap...
    waitForBufferSwaps();
    // ... and for the slaves to sync
    for (int p = 0; p < _slaveProcesses.size(); p++) {
        QVREvent e;
        _slaveProcesses[p]->waitForSlaveData();
        while (_slaveProcesses[p]->receiveCmdEvent(&e)) {
            QVR_FIREHOSE("  ... got an event from process %d window %d",
                    e.context.processIndex(), e.context.windowIndex());
            eventQueue->enqueue(e);
        }
        if (!_slaveProcesses[p]->receiveCmdSync()) {
            _wantExit = true;
        }
    }

    _fpsCounter++;
}

void QVRManager::slaveLoop()
{
    char cmd;
    bool ok = _thisProcess->receiveCmd(&cmd);
    if (!ok) {
        QVR_FATAL("  no command from master?!");
        return;
    }
    QVR_FIREHOSE("  ... got command %c from master", cmd);
    if (cmd == 'w') {
        _thisProcess->receiveCmdWasdqeState(&_wasdqeMouseProcessIndex,
                &_wasdqeMouseWindowIndex, &_wasdqeMouseInitialized);
    } else if (cmd == 'o') {
        QVRObserver o;
        _thisProcess->receiveCmdObserver(&o);
        *(_observers.at(o.index())) = o;
    } else if (cmd == 'r') {
        _thisProcess->receiveCmdRender(&_near, &_far, _app);
        render();
        while (!eventQueue->empty()) {
            QVR_FIREHOSE("  ... sending event to master process");
            _thisProcess->sendCmdEvent(&eventQueue->front());
            eventQueue->dequeue();
        }
        QVR_FIREHOSE("  ... sending sync to master");
        _thisProcess->sendCmdSync();
        _thisProcess->flush();
        waitForBufferSwaps();
    } else if (cmd == 'q') {
        _triggerTimer->stop();
        quit();
    } else {
        QVR_FATAL("  unknown command from master!?");
    }
}

void QVRManager::quit()
{
    QVR_DEBUG("quitting process %d...", _thisProcess->index());
    if (_masterWindow)
        _masterWindow->winContext()->makeCurrent(_masterWindow);

    for (int w = _windows.size() - 1; w >= 0; w--) {
        QVR_DEBUG("... exiting window %d", w);
        _app->exitWindow(_windows[w]);
        _windows[w]->exitGL();
        _windows[w]->close();
    }
    QVR_DEBUG("... exiting process");
    _app->exitProcess(_thisProcess);

    QVR_DEBUG("... quitting process %d done", _thisProcess->index());
}

void QVRManager::render()
{
    QApplication::processEvents();
    QVR_FIREHOSE("  render() ...");

    if (_masterWindow)
        _masterWindow->winContext()->makeCurrent(_masterWindow);

    if (_windows.size() > 0) {
        QVR_FIREHOSE("  ... preRenderProcess()");
        _app->preRenderProcess(_thisProcess);
        // render
        for (int w = 0; w < _windows.size(); w++) {
            QVR_FIREHOSE("  ... preRenderWindow(%d)", w);
            if (!_wasdqeMouseInitialized) {
                if (_wasdqeMouseProcessIndex == _windows[w]->processIndex()
                        && _wasdqeMouseWindowIndex == _windows[w]->index()) {
                    _windows[w]->setCursor(Qt::BlankCursor);
                    QCursor::setPos(_windows[w]->mapToGlobal(
                                QPoint(_windows[w]->width() / 2, _windows[w]->height() / 2)));
                } else {
                    _windows[w]->unsetCursor();
                }
            }
            _app->preRenderWindow(_windows[w]);
            QVR_FIREHOSE("  ... render(%d)", w);
            unsigned int textures[2];
            const QVRRenderContext& renderContext = _windows[w]->computeRenderContext(_near, _far, textures);
            for (int i = 0; i < renderContext.viewPasses(); i++) {
                QVR_FIREHOSE("  ... pass %d", i);
                QVR_FIREHOSE("  ...   frustum: l=%g r=%g b=%g t=%g n=%g f=%g",
                        renderContext.frustum(i).left(),
                        renderContext.frustum(i).right(),
                        renderContext.frustum(i).bottom(),
                        renderContext.frustum(i).top(),
                        renderContext.frustum(i).near(),
                        renderContext.frustum(i).far());
                QVR_FIREHOSE("  ...   viewmatrix: [%g %g %g %g] [%g %g %g %g] [%g %g %g %g] [%g %g %g %g]",
                        renderContext.viewMatrix(i)(0, 0), renderContext.viewMatrix(i)(0, 1),
                        renderContext.viewMatrix(i)(0, 2), renderContext.viewMatrix(i)(0, 3),
                        renderContext.viewMatrix(i)(1, 0), renderContext.viewMatrix(i)(1, 1),
                        renderContext.viewMatrix(i)(1, 2), renderContext.viewMatrix(i)(1, 3),
                        renderContext.viewMatrix(i)(2, 0), renderContext.viewMatrix(i)(2, 1),
                        renderContext.viewMatrix(i)(2, 2), renderContext.viewMatrix(i)(2, 3),
                        renderContext.viewMatrix(i)(3, 0), renderContext.viewMatrix(i)(3, 1),
                        renderContext.viewMatrix(i)(3, 2), renderContext.viewMatrix(i)(3, 3));
                _app->render(_windows[w], renderContext, i, textures[i]);
            }
            QVR_FIREHOSE("  ... postRenderWindow(%d)", w);
            _app->postRenderWindow(_windows[w]);
        }
        _wasdqeMouseInitialized = true;
        QVR_FIREHOSE("  ... postRenderProcess()");
        _app->postRenderProcess(_thisProcess);
        /* At this point, we must make sure that all textures actually contain
         * the current scene, otherwise artefacts are displayed when the window
         * threads render them. It seems that glFlush() is not enough for all
         * OpenGL implementations; to be safe, we use glFinish(). */
        glFinish();
        for (int w = 0; w < _windows.size(); w++) {
            QVR_FIREHOSE("  ... renderToScreen(%d)", w);
            _windows[w]->renderToScreen();
        }
        for (int w = 0; w < _windows.size(); w++) {
            QVR_FIREHOSE("  ... asyncSwapBuffers(%d)", w);
            _windows[w]->asyncSwapBuffers();
        }
    }
}

void QVRManager::waitForBufferSwaps()
{
    // wait for windows to finish the buffer swap
    for (int w = 0; w < _windows.size(); w++) {
        QVR_FIREHOSE("  ... waitForSwapBuffers(%d)", w);
        _windows[w]->waitForSwapBuffers();
    }
}

void QVRManager::printFps()
{
    if (_fpsCounter > 0) {
        QVR_FATAL("fps %.1f", _fpsCounter / (_fpsMsecs / 1000.0f));
        _fpsCounter = 0;
    }
}

QVRManager* QVRManager::instance()
{
    return manager;
}

const QVRConfig& QVRManager::config()
{
    Q_ASSERT(manager);
    return *(manager->_config);
}

const QVRObserverConfig& QVRManager::observerConfig(int observerIndex)
{
    return config().observerConfigs().at(observerIndex);
}

const QVRProcessConfig& QVRManager::processConfig(int processIndex)
{
    return config().processConfigs().at(processIndex);
}

const QVRWindowConfig& QVRManager::windowConfig(int processIndex, int windowIndex)
{
    return processConfig(processIndex).windowConfigs().at(windowIndex);
}

QVRLogLevel QVRManager::logLevel()
{
    Q_ASSERT(manager);
    return manager->_logLevel;
}

int QVRManager::processIndex()
{
    Q_ASSERT(manager);
    return manager->_processIndex;
}

void QVRManager::enqueueKeyPressEvent(const QVRRenderContext& c, QKeyEvent* event)
{
    eventQueue->enqueue(QVREvent(QVR_Event_KeyPress, c, *event));
}

void QVRManager::enqueueKeyReleaseEvent(const QVRRenderContext& c, QKeyEvent* event)
{
    eventQueue->enqueue(QVREvent(QVR_Event_KeyRelease, c, *event));
}

void QVRManager::enqueueMouseMoveEvent(const QVRRenderContext& c, QMouseEvent* event)
{
    eventQueue->enqueue(QVREvent(QVR_Event_MouseMove, c, *event));
}

void QVRManager::enqueueMousePressEvent(const QVRRenderContext& c, QMouseEvent* event)
{
    eventQueue->enqueue(QVREvent(QVR_Event_MousePress, c, *event));
}

void QVRManager::enqueueMouseReleaseEvent(const QVRRenderContext& c, QMouseEvent* event)
{
    eventQueue->enqueue(QVREvent(QVR_Event_MouseRelease, c, *event));
}

void QVRManager::enqueueMouseDoubleClickEvent(const QVRRenderContext& c, QMouseEvent* event)
{
    eventQueue->enqueue(QVREvent(QVR_Event_MouseDoubleClick, c, *event));
}

void QVRManager::enqueueWheelEvent(const QVRRenderContext& c, QWheelEvent* event)
{
    eventQueue->enqueue(QVREvent(QVR_Event_Wheel, c, *event));
}

void QVRManager::processEventQueue()
{
    while (!eventQueue->empty()) {
        QVREvent e = eventQueue->front();
        eventQueue->dequeue();
        if (_haveWasdqeObservers
                && observerConfig(windowConfig(e.context.processIndex(), e.context.windowIndex())
                    .observerIndex()).navigationType() == QVR_Navigation_WASDQE) {
            bool consumed = false;
            if (e.type == QVR_Event_KeyPress) {
                switch (e.keyEvent.key()) {
                case Qt::Key_Escape:
                    if (_wasdqeMouseProcessIndex >= 0) {
                        _wasdqeMouseProcessIndex = -1;
                        _wasdqeMouseWindowIndex = -1;
                        _wasdqeMouseInitialized = false;
                        consumed = true;
                    }
                    break;
                case Qt::Key_W:
                    _wasdqeIsPressed[0] = true;
                    consumed = true;
                    break;
                case Qt::Key_A:
                    _wasdqeIsPressed[1] = true;
                    consumed = true;
                    break;
                case Qt::Key_S:
                    _wasdqeIsPressed[2] = true;
                    consumed = true;
                    break;
                case Qt::Key_D:
                    _wasdqeIsPressed[3] = true;
                    consumed = true;
                    break;
                case Qt::Key_Q:
                    _wasdqeIsPressed[4] = true;
                    consumed = true;
                    break;
                case Qt::Key_E:
                    _wasdqeIsPressed[5] = true;
                    consumed = true;
                    break;
                }
            } else if (e.type == QVR_Event_KeyRelease) {
                switch (e.keyEvent.key())
                {
                case Qt::Key_W:
                    _wasdqeIsPressed[0] = false;
                    consumed = true;
                    break;
                case Qt::Key_A:
                    _wasdqeIsPressed[1] = false;
                    consumed = true;
                    break;
                case Qt::Key_S:
                    _wasdqeIsPressed[2] = false;
                    consumed = true;
                    break;
                case Qt::Key_D:
                    _wasdqeIsPressed[3] = false;
                    consumed = true;
                    break;
                case Qt::Key_Q:
                    _wasdqeIsPressed[4] = false;
                    consumed = true;
                    break;
                case Qt::Key_E:
                    _wasdqeIsPressed[5] = false;
                    consumed = true;
                    break;
                }
            } else if (e.type == QVR_Event_MousePress) {
                _wasdqeMouseProcessIndex = e.context.processIndex();
                _wasdqeMouseWindowIndex = e.context.windowIndex();
                _wasdqeMouseInitialized = false;
                consumed = true;
            } else if (e.type == QVR_Event_MouseMove) {
                if (_wasdqeMouseInitialized
                        && _wasdqeMouseProcessIndex == e.context.processIndex()
                        && _wasdqeMouseWindowIndex == e.context.windowIndex()) {
                    // Horizontal angle
                    float x = e.mouseEvent.pos().x();
                    float w = e.context.windowGeometry().width();
                    float xf = x / w * 2.0f - 1.0f;
                    _wasdqeHorzAngle = -xf * 180.0f;
                    // Vertical angle
                    // For HMDs, up/down views are realized via head movements. Additional
                    // mouse-based up/down views should be disabled since they lead to
                    // sickness fast ;)
                    if (windowConfig(e.context.processIndex(), e.context.windowIndex()).outputMode()
                            != QVR_Output_Stereo_Oculus) {
                        float y = e.mouseEvent.pos().y();
                        float h = e.context.windowGeometry().height();
                        float yf = y / h * 2.0f - 1.0f;
                        _wasdqeVertAngle = -yf * 90.0f;
                    }
                    consumed = true;
                }
            }
            if (consumed) {
                continue;
            }
        }
        switch (e.type) {
        case QVR_Event_KeyPress:
            _app->keyPressEvent(e.context, &e.keyEvent);
            break;
        case QVR_Event_KeyRelease:
            _app->keyReleaseEvent(e.context, &e.keyEvent);
            break;
        case QVR_Event_MouseMove:
            _app->mouseMoveEvent(e.context, &e.mouseEvent);
            break;
        case QVR_Event_MousePress:
            _app->mousePressEvent(e.context, &e.mouseEvent);
            break;
        case QVR_Event_MouseRelease:
            _app->mouseReleaseEvent(e.context, &e.mouseEvent);
            break;
        case QVR_Event_MouseDoubleClick:
            _app->mouseDoubleClickEvent(e.context, &e.mouseEvent);
            break;
        case QVR_Event_Wheel:
            _app->wheelEvent(e.context, &e.wheelEvent);
            break;
        }
    }
}
