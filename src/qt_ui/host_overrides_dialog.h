// SPDX-FileCopyrightText: Copyright 2026 shadLauncher4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <functional>
#include <QDialog>
#include <QString>

class QTableWidget;
class QLabel;
class QPushButton;
class QWidget;

// Result of fetching a game's published host overrides from the shad-net repository.
struct HostOverridesDownloadResult {
    enum class Status {
        Downloaded,   // new file written to host_overrides/<TITLE_ID>.json
        NotPublished, // the repository has no file for this title
        Failed,       // network, HTTP, validation or write error (see message)
    };
    Status status = Status::Failed;
    QString message;
};

class HostOverridesDialog : public QDialog {
    Q_OBJECT
public:
    explicit HostOverridesDialog(QWidget* parent = nullptr);
    HostOverridesDialog(const QString& title_id, const QString& game_name,
                        QWidget* parent = nullptr);
    static std::filesystem::path FilePath();
    static std::filesystem::path PerGameDir();
    static std::filesystem::path PerGameFilePath(const QString& title_id);
    static bool IsValidTitleId(const QString& title_id);
    static void DownloadForTitle(const QString& title_id, QObject* context,
                                 std::function<void(const HostOverridesDownloadResult&)> on_done,
                                 bool overwrite_user_file = false);
    static bool HasUserWrittenFile(const QString& title_id);

private:
    void Init();
    std::filesystem::path CurrentPath() const;
    bool IsPerGame() const {
        return !m_title_id.isEmpty();
    }
    void OnDownload();
    void AddRow(const QString& match, const QString& target, bool enabled);
    void OnAdd();
    void OnRemove();
    void OnAddCatchAll();
    void OnImport();
    void OnExport();
    void LoadFromDisk();
    bool SaveToDisk();

    QString m_title_id;
    QString m_game_name;
    QTableWidget* m_table = nullptr;
    QLabel* m_path_label = nullptr;
    QLabel* m_source_label = nullptr;
    QPushButton* m_download_btn = nullptr;
};
