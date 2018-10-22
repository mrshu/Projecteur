#pragma once

#include <QDialog>

class QComboBox;
class Settings;

class PreferencesDialog : public QDialog
{
  Q_OBJECT
  Q_PROPERTY(bool dialogActive READ dialogActive NOTIFY dialogActiveChanged)

public:
  explicit PreferencesDialog(Settings* settings, QWidget* parent = nullptr);
  virtual ~PreferencesDialog() override = default;

  bool dialogActive() const { return m_active; }

public slots:
  void updateAvailableScreens(QList<QScreen*> screens);

signals:
  void dialogActiveChanged(bool active);

protected:
  virtual bool event(QEvent* event) override;

private:
  void setDialogActive(bool active);

private:
  bool m_active = false;
  QComboBox* m_screenCb = nullptr;
};
