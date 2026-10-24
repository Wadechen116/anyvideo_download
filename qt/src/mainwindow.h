#pragma once

#include <QMainWindow>
#include <QPointer>

#include "engine.h"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QTextEdit;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildUi();
    void loadSettingsIntoUi();
    avd::Settings collectSettings();
    void updateOptionStates();
    void refreshEnvLabel();
    void detectEngineAsync();
    void onEngineDetected(const avd::Engine& engine, bool firstTime);

    QStringList urls() const;
    void pasteUrls();
    void chooseDir();
    void chooseCookiesFile();
    void openDir();
    void probe();
    void startDownload();
    bool guardBeforeDownload(avd::Settings& s);
    void cancelDownload();
    void updateCore();
    void captureFromBrowser(const QString& startUrl = QString());
    void setBusy(bool busy);

    void onProgress(double pct, const QString& text);
    void onStatus(const QString& text);
    void onDone(int ok, int fail, bool cancelled);
    void setBar(double pct);  // pct < 0：來回跑的進度條
    void appendLog(const QString& text, const QString& tag = "info");

    avd::Settings settings_;
    avd::Engine engine_;
    QString ffmpegDir_;
    bool engineKnown_ = false;
    bool busy_ = false;
    bool probing_ = false;
    bool updating_ = false;
    QPointer<avd::DownloadJob> job_;

    QPlainTextEdit* urlEdit_ = nullptr;
    QPushButton* btnProbe_ = nullptr;
    QRadioButton* rbVideo_ = nullptr;
    QRadioButton* rbAudio_ = nullptr;
    QComboBox* cbContainer_ = nullptr;
    QComboBox* cbQuality_ = nullptr;
    QComboBox* cbPriority_ = nullptr;
    QComboBox* cbAudioFormat_ = nullptr;
    QComboBox* cbBitrate_ = nullptr;
    QCheckBox* chkPlaylist_ = nullptr;
    QCheckBox* chkMeta_ = nullptr;
    QCheckBox* chkThumb_ = nullptr;
    QCheckBox* chkSubs_ = nullptr;
    QComboBox* cbBrowser_ = nullptr;
    QLineEdit* cookiesFile_ = nullptr;
    QPushButton* btnCookiesPick_ = nullptr;
    QPushButton* btnCookiesClear_ = nullptr;
    QLineEdit* outDir_ = nullptr;
    QPushButton* btnBrowse_ = nullptr;
    QPushButton* btnStart_ = nullptr;
    QPushButton* btnCancel_ = nullptr;
    QPushButton* btnUpdate_ = nullptr;
    QPushButton* btnCapture_ = nullptr;
    QLabel* lblItem_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* lblStatus_ = nullptr;
    QTextEdit* log_ = nullptr;
    QLabel* lblEnv_ = nullptr;
};
