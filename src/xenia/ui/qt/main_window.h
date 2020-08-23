#ifndef XENIA_UI_QT_MAINWINDOW_H_
#define XENIA_UI_QT_MAINWINDOW_H_

#include <QMainWindow>

#include "window_qt.h"
#include "xenia/ui/qt/themeable_widget.h"
#include "xenia/ui/qt/widgets/shell.h"

namespace xe {
namespace ui {
namespace qt {
class XStatusBar;

class MainWindow final : public Themeable<QtWindow> {
  Q_OBJECT
 public:
  MainWindow(Loop* loop, const std::string& title)
      : Themeable<QtWindow>("MainWindow", loop, title) {}

  bool Initialize() override;

  void AddStatusBarWidget(QWidget* widget, bool permanent = false);
  void RemoveStatusBarWidget(QWidget* widget);

  const XStatusBar* status_bar() const { return status_bar_; }

 private:
  XShell* shell_ = nullptr;
  XStatusBar* status_bar_ = nullptr;
};

}  // namespace qt
}  // namespace ui
}  // namespace xe

#endif