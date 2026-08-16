#pragma once

#include <QApplication>

#include "util/duration.h"

class MixxxApplication : public QApplication {
    Q_OBJECT
  public:
    MixxxApplication(int& argc, char** argv);
    ~MixxxApplication() override = default;

    bool notify(QObject*, QEvent*) override;

    void setNotifyWarningThreshold(int threshold);

  private:
    void registerMetaTypes();

    bool m_isDeveloper;
    mixxx::Duration m_eventNotifyExecTimeWarningThreshold;
};
