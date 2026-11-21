// SPDX-FileCopyrightText: Copyright 2026 shadLauncher4 Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <set>

#include <QAbstractButton>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <nlohmann/json.hpp>

#include "common/path_util.h"
#include "common/scm_rev.h"
#include "host_overrides_dialog.h"

namespace {

// Published per-game overrides: <RepoBaseUrl><TITLE_ID>.json
constexpr const char* RepoBaseUrl =
    "https://raw.githubusercontent.com/shad-net/http_host_overrides/main/";
constexpr const char* RepoPageUrl = "https://github.com/shad-net/http_host_overrides";
// Same limits the emulator applies to downloaded files.
constexpr qint64 MaxFileSize = 256 * 1024;
constexpr int TransferTimeoutMs = 15000;

std::filesystem::path EtagPathFor(const std::filesystem::path& json_path) {
    auto p = json_path;
    p += ".etag";
    return p;
}

bool WriteFileAtomic(const std::filesystem::path& path, const QByteArray& data) {
    QString qpath;
    Common::FS::PathToQString(qpath, path);
    QSaveFile f(qpath);
    if (!f.open(QIODevice::WriteOnly)) {
        return false;
    }
    if (f.write(data) != data.size()) {
        f.cancelWriting();
        return false;
    }
    return f.commit();
}

void RemoveIfExists(const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

constexpr int ColEnabled = 0;
constexpr int ColMatch = 1;
constexpr int ColTarget = 2;

QString ValidateTarget(const QString& raw) {
    QString value = raw.trimmed();
    if (value.isEmpty()) {
        return QObject::tr("Redirect target is empty.");
    }
    // Optional scheme.
    if (const int sep = value.indexOf("://"); sep != -1) {
        const QString scheme = value.left(sep).toLower();
        if (scheme != "http" && scheme != "https") {
            return QObject::tr("Target scheme must be http or https.");
        }
        value = value.mid(sep + 3);
    }
    if (value.isEmpty()) {
        return QObject::tr("Redirect target has no host.");
    }
    // Optional :port
    if (const int colon = value.indexOf(':'); colon != -1) {
        const QString host = value.left(colon);
        const QString port_str = value.mid(colon + 1);
        if (host.isEmpty()) {
            return QObject::tr("Redirect target has no host.");
        }
        bool ok = false;
        const int port = port_str.toInt(&ok);
        if (!ok || port < 1 || port > 65535) {
            return QObject::tr("Port must be a number between 1 and 65535.");
        }
    }
    return {};
}

QString ValidateMatch(const QString& raw) {
    const QString key = raw.trimmed();
    if (key.isEmpty()) {
        return QObject::tr("Match host is empty.");
    }
    if (key == "*") {
        return {};
    }
    QString rest = key;
    if (const int sep = rest.indexOf("://"); sep != -1) {
        const QString scheme = rest.left(sep).toLower();
        if (scheme != "http" && scheme != "https") {
            return QObject::tr("Match scheme must be http or https (or omit it).");
        }
        rest = rest.mid(sep + 3);
    }
    if (rest.isEmpty()) {
        return QObject::tr("Match has no host.");
    }
    if (const int colon = rest.indexOf(':'); colon != -1) {
        bool ok = false;
        const int port = rest.mid(colon + 1).toInt(&ok);
        if (!ok || port < 1 || port > 65535) {
            return QObject::tr("Match port must be a number between 1 and 65535.");
        }
    }
    return {};
}

struct OverrideEntry {
    QString match;
    QString target;
    bool enabled;
};

std::vector<OverrideEntry> ParseOverridesJson(const nlohmann::ordered_json& root) {
    std::vector<OverrideEntry> entries;
    for (auto it = root.begin(); it != root.end(); ++it) {
        if (!it.value().is_string()) {
            continue;
        }
        QString key = QString::fromStdString(it.key());
        bool enabled = true;
        if (key.startsWith('_')) {
            enabled = false;
            key.remove(0, 1);
        }
        entries.push_back(OverrideEntry{
            std::move(key), QString::fromStdString(it.value().get<std::string>()), enabled});
    }
    return entries;
}

nlohmann::ordered_json BuildOverridesJson(const QTableWidget* table, QStringList& problems) {
    nlohmann::ordered_json root = nlohmann::ordered_json::object();
    std::set<QString> seen_keys;

    for (int r = 0; r < table->rowCount(); ++r) {
        const QTableWidgetItem* match_item = table->item(r, ColMatch);
        const QTableWidgetItem* target_item = table->item(r, ColTarget);
        const QTableWidgetItem* check_item = table->item(r, ColEnabled);

        const QString match = match_item ? match_item->text().trimmed() : QString();
        const QString target = target_item ? target_item->text().trimmed() : QString();
        const bool enabled = check_item && check_item->checkState() == Qt::Checked;

        if (match.isEmpty() && target.isEmpty()) {
            continue; // skip blank rows silently
        }

        if (const QString err = ValidateMatch(match); !err.isEmpty()) {
            problems << QObject::tr("Row %1: %2").arg(r + 1).arg(err);
            continue;
        }
        if (const QString err = ValidateTarget(target); !err.isEmpty()) {
            problems << QObject::tr("Row %1: %2").arg(r + 1).arg(err);
            continue;
        }

        // Disabled rows are persisted with a leading '_' so the loader skips them.
        const QString effective_key = enabled ? match : QStringLiteral("_") + match;
        if (seen_keys.count(effective_key)) {
            problems << QObject::tr("Row %1: duplicate entry for '%2'.").arg(r + 1).arg(match);
            continue;
        }
        seen_keys.insert(effective_key);
        root[effective_key.toStdString()] = target.toStdString();
    }

    return root;
}

} // namespace

std::filesystem::path HostOverridesDialog::FilePath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "host_overrides.json";
}

std::filesystem::path HostOverridesDialog::PerGameDir() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "host_overrides";
}

std::filesystem::path HostOverridesDialog::PerGameFilePath(const QString& title_id) {
    return PerGameDir() / (title_id.toStdString() + ".json");
}

bool HostOverridesDialog::IsValidTitleId(const QString& title_id) {
    static const QRegularExpression re(QStringLiteral("^[A-Z]{4}[0-9]{5}$"));
    return re.match(title_id).hasMatch();
}

bool HostOverridesDialog::HasUserWrittenFile(const QString& title_id) {
    if (!IsValidTitleId(title_id)) {
        return false;
    }
    std::error_code ec;
    const auto json_path = PerGameFilePath(title_id);
    return std::filesystem::exists(json_path, ec) &&
           !std::filesystem::exists(EtagPathFor(json_path), ec);
}

void HostOverridesDialog::DownloadForTitle(
    const QString& title_id, QObject* context,
    std::function<void(const HostOverridesDownloadResult&)> on_done, bool overwrite_user_file) {
    using Status = HostOverridesDownloadResult::Status;
    auto finish = [on_done](Status status, const QString& message) {
        if (on_done) {
            on_done(HostOverridesDownloadResult{status, message});
        }
    };

    if (!IsValidTitleId(title_id)) {
        finish(Status::Failed, tr("'%1' is not a valid title id.").arg(title_id));
        return;
    }
    if (!overwrite_user_file && HasUserWrittenFile(title_id)) {
        finish(Status::Failed,
               tr("%1 already has locally edited host overrides; they were left unchanged.")
                   .arg(title_id));
        return;
    }

    auto* manager = new QNetworkAccessManager(context);
    QNetworkRequest request(QUrl(QString::fromLatin1(RepoBaseUrl) + title_id + ".json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("shadLauncher4/%1").arg(QString::fromLatin1(Common::g_version)));
    request.setTransferTimeout(TransferTimeoutMs);

    QNetworkReply* reply = manager->get(request);
    QObject::connect(reply, &QNetworkReply::finished, manager, [=]() {
        reply->deleteLater();
        manager->deleteLater();

        const int http_status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto json_path = PerGameFilePath(title_id);
        const auto etag_path = EtagPathFor(json_path);

        if (http_status == 404) {
            // Mirror the emulator: a previously downloaded copy that was removed
            // upstream is deleted; a user-written file is never touched.
            std::error_code ec;
            if (std::filesystem::exists(etag_path, ec)) {
                RemoveIfExists(json_path);
                RemoveIfExists(etag_path);
            }
            finish(Status::NotPublished,
                   tr("No host overrides are published for %1 yet.").arg(title_id));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            finish(Status::Failed, tr("Download failed: %1").arg(reply->errorString()));
            return;
        }
        if (http_status != 200) {
            finish(Status::Failed,
                   tr("Download failed: unexpected HTTP status %1.").arg(http_status));
            return;
        }

        const QByteArray body = reply->readAll();
        if (body.size() > MaxFileSize) {
            finish(Status::Failed,
                   tr("The published file is too large (%1 bytes).").arg(body.size()));
            return;
        }
        try {
            if (!nlohmann::ordered_json::parse(body.toStdString()).is_object()) {
                finish(Status::Failed, tr("The published file is not a JSON object."));
                return;
            }
        } catch (const std::exception&) {
            finish(Status::Failed, tr("The published file is not valid JSON."));
            return;
        }

        std::error_code ec;
        std::filesystem::create_directories(json_path.parent_path(), ec);
        if (!WriteFileAtomic(json_path, body)) {
            QString p;
            Common::FS::PathToQString(p, json_path);
            finish(Status::Failed, tr("Could not write %1").arg(p));
            return;
        }
        WriteFileAtomic(etag_path, reply->rawHeader("ETag"));
        finish(Status::Downloaded, tr("Downloaded host overrides for %1.").arg(title_id));
    });
}

HostOverridesDialog::HostOverridesDialog(QWidget* parent) : QDialog(parent) {
    Init();
}

HostOverridesDialog::HostOverridesDialog(const QString& title_id, const QString& game_name,
                                         QWidget* parent)
    : QDialog(parent), m_title_id(title_id), m_game_name(game_name) {
    Init();
}

std::filesystem::path HostOverridesDialog::CurrentPath() const {
    return IsPerGame() ? PerGameFilePath(m_title_id) : FilePath();
}

void HostOverridesDialog::Init() {
    if (IsPerGame()) {
        setWindowTitle(m_game_name.isEmpty()
                           ? tr("Network Host Overrides - %1").arg(m_title_id)
                           : tr("Network Host Overrides - %1 (%2)").arg(m_game_name, m_title_id));
    } else {
        setWindowTitle(tr("Network Host Overrides"));
    }
    setObjectName("HostOverridesDialog");
    setModal(true);
    setMinimumSize(640, 420);

    auto* intro = new QLabel(this);
    intro->setObjectName("HostOverridesDialog_intro");
    intro->setWordWrap(true);
    intro->setTextFormat(Qt::RichText);
    intro->setText(tr(
        "Redirect a game's network requests to a different server (for example a "
        "local private server). Each row maps a <b>match</b> to a <b>redirect target</b>. "
        "Matches are tried most-specific first:<br>"
        "&nbsp;&nbsp;<tt>host</tt> &middot; <tt>host:port</tt> &middot; "
        "<tt>scheme://host:port</tt> &middot; <tt>*</tt> (catch-all)<br>"
        "Targets look like <tt>host</tt>, <tt>host:port</tt>, or "
        "<tt>http(s)://host[:port]</tt>. Unchecked rows are saved but ignored by the emulator."));
    if (!IsPerGame()) {
        intro->setText(intro->text() + QStringLiteral("<br><br>") +
                       tr("To set or download overrides for a single game, right-click it in "
                          "the game list and choose <b>Network Host Overrides</b>."));
    } else {
        intro->setText(
            intro->text() + QStringLiteral("<br><br>") +
            tr("These overrides apply only to this game and replace the global list while it "
               "runs. Use <b>Download from shad-net</b> to get the overrides published for this "
               "game at <a href=\"%1\">shad-net/http_host_overrides</a>. Saving an empty list "
               "removes the per-game file, so the global list is used again.")
                .arg(QString::fromLatin1(RepoPageUrl)));
        intro->setOpenExternalLinks(true);
    }

    // Table
    m_table = new QTableWidget(this);
    m_table->setObjectName("HostOverridesDialog_table");
    m_table->setColumnCount(3);
    m_table->setHorizontalHeaderLabels(
        {tr("Enabled"), tr("Match (host / endpoint)"), tr("Redirect to")});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(ColMatch, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(ColEnabled, QHeaderView::ResizeToContents);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);

    // Editing buttons
    auto* add_btn = new QPushButton(tr("&Add"), this);
    auto* add_catchall_btn = new QPushButton(tr("Add &catch-all"), this);
    auto* remove_btn = new QPushButton(tr("&Remove"), this);
    auto* import_btn = new QPushButton(tr("&Import..."), this);
    auto* export_btn = new QPushButton(tr("&Export..."), this);
    add_btn->setAutoDefault(false);
    add_catchall_btn->setAutoDefault(false);
    remove_btn->setAutoDefault(false);
    import_btn->setAutoDefault(false);
    export_btn->setAutoDefault(false);

    auto* edit_row = new QHBoxLayout();
    edit_row->addWidget(add_btn);
    edit_row->addWidget(add_catchall_btn);
    edit_row->addWidget(remove_btn);
    edit_row->addWidget(import_btn);
    edit_row->addWidget(export_btn);
    edit_row->addStretch();

    connect(add_btn, &QPushButton::clicked, this, &HostOverridesDialog::OnAdd);
    connect(add_catchall_btn, &QPushButton::clicked, this, &HostOverridesDialog::OnAddCatchAll);
    connect(remove_btn, &QPushButton::clicked, this, &HostOverridesDialog::OnRemove);
    connect(import_btn, &QPushButton::clicked, this, &HostOverridesDialog::OnImport);
    connect(export_btn, &QPushButton::clicked, this, &HostOverridesDialog::OnExport);

    // Path label + open-folder
    m_path_label = new QLabel(this);
    m_path_label->setObjectName("HostOverridesDialog_path");
    m_path_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    {
        QString p;
        Common::FS::PathToQString(p, CurrentPath());
        m_path_label->setText(tr("File: %1").arg(p));
    }
    m_source_label = new QLabel(this);
    m_source_label->setObjectName("HostOverridesDialog_source");
    m_source_label->setWordWrap(true);
    m_source_label->setVisible(IsPerGame());
    {
        QFont f = m_path_label->font();
        f.setPointSizeF(f.pointSizeF() - 1.0);
        m_path_label->setFont(f);
    }

    // Dialog buttons
    auto* buttons = new QDialogButtonBox(this);
    auto* open_btn = buttons->addButton(tr("Open Folder"), QDialogButtonBox::ActionRole);
    if (IsPerGame()) {
        m_download_btn =
            buttons->addButton(tr("&Download from shad-net"), QDialogButtonBox::ActionRole);
        m_download_btn->setAutoDefault(false);
        connect(m_download_btn, &QPushButton::clicked, this, &HostOverridesDialog::OnDownload);
    }
    auto* save_btn = buttons->addButton(QDialogButtonBox::Save);
    buttons->addButton(QDialogButtonBox::Close);
    save_btn->setDefault(true);

    connect(open_btn, &QPushButton::clicked, this, [this]() {
        QString dir;
        std::error_code ec;
        std::filesystem::create_directories(CurrentPath().parent_path(), ec);
        Common::FS::PathToQString(dir, CurrentPath().parent_path());
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        if (SaveToDisk()) {
            accept();
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto* root = new QVBoxLayout(this);
    root->addWidget(intro);
    root->addWidget(m_table, 1);
    root->addLayout(edit_row);
    root->addWidget(m_source_label);
    root->addWidget(m_path_label);
    root->addWidget(buttons);
    setLayout(root);

    LoadFromDisk();
}

void HostOverridesDialog::AddRow(const QString& match, const QString& target, bool enabled) {
    const int row = m_table->rowCount();
    m_table->insertRow(row);

    auto* check = new QTableWidgetItem();
    check->setFlags((check->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
    check->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    check->setTextAlignment(Qt::AlignCenter);
    m_table->setItem(row, ColEnabled, check);

    m_table->setItem(row, ColMatch, new QTableWidgetItem(match));
    m_table->setItem(row, ColTarget, new QTableWidgetItem(target));
}

void HostOverridesDialog::OnAdd() {
    AddRow(QString(), QStringLiteral("http://localhost:8080"), true);
    m_table->editItem(m_table->item(m_table->rowCount() - 1, ColMatch));
}

void HostOverridesDialog::OnAddCatchAll() {
    // Don't add a second catch-all if one already exists.
    for (int r = 0; r < m_table->rowCount(); ++r) {
        if (m_table->item(r, ColMatch) && m_table->item(r, ColMatch)->text().trimmed() == "*") {
            QMessageBox::information(this, tr("Catch-all exists"),
                                     tr("A catch-all (*) row already exists."));
            return;
        }
    }
    AddRow(QStringLiteral("*"), QStringLiteral("http://localhost:8080"), true);
}

void HostOverridesDialog::OnRemove() {
    // Collect unique selected rows, delete from the bottom up.
    std::set<int, std::greater<int>> rows;
    for (const QModelIndex& idx : m_table->selectionModel()->selectedRows()) {
        rows.insert(idx.row());
    }
    if (rows.empty()) {
        if (const int cur = m_table->currentRow(); cur >= 0) {
            rows.insert(cur);
        }
    }
    for (const int r : rows) {
        m_table->removeRow(r);
    }
}

void HostOverridesDialog::OnImport() {
    const QString file_name = QFileDialog::getOpenFileName(
        this, tr("Import Host Overrides"), QString(), tr("JSON Files (*.json);;All Files (*)"));
    if (file_name.isEmpty()) {
        return;
    }

    std::ifstream in(file_name.toStdString());
    if (!in.is_open()) {
        QMessageBox::critical(this, tr("Import failed"), tr("Could not open the selected file."));
        return;
    }

    nlohmann::ordered_json root;
    try {
        in >> root;
    } catch (const std::exception&) {
        QMessageBox::warning(this, tr("Parse error"),
                             tr("The selected file could not be parsed as JSON."));
        return;
    }
    if (!root.is_object()) {
        QMessageBox::warning(
            this, tr("Format error"),
            tr("The selected file must be a JSON object mapping match keys to redirect "
               "targets, like host_overrides.json."));
        return;
    }

    const std::vector<OverrideEntry> entries = ParseOverridesJson(root);
    if (entries.empty()) {
        QMessageBox::information(this, tr("Nothing to import"),
                                 tr("The selected file doesn't contain any valid entries."));
        return;
    }

    QMessageBox box(this);
    box.setWindowTitle(tr("Import Host Overrides"));
    box.setIcon(QMessageBox::Question);
    box.setText(tr("Found %1 entry/entries in the selected file.\n\n"
                   "Replace the current list entirely, or append these entries to it "
                   "(entries whose match already exists in the list will be skipped)?")
                    .arg(entries.size()));
    QPushButton* replace_btn = box.addButton(tr("Replace All"), QMessageBox::DestructiveRole);
    QPushButton* append_btn = box.addButton(tr("Append"), QMessageBox::AcceptRole);
    box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(append_btn);
    box.exec();

    const QAbstractButton* clicked = box.clickedButton();
    if (clicked == replace_btn) {
        m_table->setRowCount(0);
        for (const OverrideEntry& entry : entries) {
            AddRow(entry.match, entry.target, entry.enabled);
        }
        QMessageBox::information(
            this, tr("Import complete"),
            tr("Replaced the list with %1 imported entry/entries.").arg(entries.size()));
    } else if (clicked == append_btn) {
        // Match keys already present in the table, so we don't create duplicate rows.
        std::set<QString> existing;
        for (int r = 0; r < m_table->rowCount(); ++r) {
            if (const QTableWidgetItem* match_item = m_table->item(r, ColMatch)) {
                existing.insert(match_item->text().trimmed());
            }
        }

        int added = 0;
        int skipped = 0;
        for (const OverrideEntry& entry : entries) {
            if (existing.count(entry.match)) {
                ++skipped;
                continue;
            }
            AddRow(entry.match, entry.target, entry.enabled);
            existing.insert(entry.match); // guard against duplicates within the imported file
            ++added;
        }

        if (skipped > 0) {
            QMessageBox::information(
                this, tr("Import complete"),
                tr("Added %1 new entry/entries. Skipped %2 already in the list.")
                    .arg(added)
                    .arg(skipped));
        } else {
            QMessageBox::information(this, tr("Import complete"),
                                     tr("Added %1 new entry/entries.").arg(added));
        }
    }
    // Cancel: leave the table untouched.
}

void HostOverridesDialog::OnExport() {
    QStringList problems;
    nlohmann::ordered_json root = BuildOverridesJson(m_table, problems);

    if (!problems.isEmpty()) {
        QMessageBox::warning(
            this, tr("Cannot export"),
            tr("Please fix the following before exporting:\n\n%1").arg(problems.join('\n')));
        return;
    }
    if (root.empty()) {
        QMessageBox::information(this, tr("Nothing to export"),
                                 tr("The list doesn't have any entries yet."));
        return;
    }

    QString default_path;
    Common::FS::PathToQString(default_path, CurrentPath());
    const QString file_name = QFileDialog::getSaveFileName(
        this, tr("Export Host Overrides"), default_path, tr("JSON Files (*.json);;All Files (*)"));
    if (file_name.isEmpty()) {
        return;
    }

    std::ofstream out(file_name.toStdString(), std::ios::trunc);
    if (!out.is_open()) {
        QMessageBox::critical(this, tr("Export failed"),
                              tr("Could not open the selected file for writing."));
        return;
    }
    out << root.dump(4) << '\n';
    if (out.fail()) {
        QMessageBox::critical(this, tr("Export failed"), tr("An error occurred while writing."));
        return;
    }

    QMessageBox::information(this, tr("Export complete"),
                             tr("Exported %1 entry/entries.").arg(root.size()));
}

void HostOverridesDialog::LoadFromDisk() {
    m_table->setRowCount(0);

    const std::filesystem::path path = CurrentPath();
    if (IsPerGame()) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            m_source_label->setText(
                tr("No per-game file yet: this game uses the global host overrides."));
        } else if (std::filesystem::exists(EtagPathFor(path), ec)) {
            m_source_label->setText(tr("Source: downloaded from shad-net. The emulator may "
                                       "refresh it automatically; saving edits makes it local."));
        } else {
            m_source_label->setText(tr("Source: local file (edited by you). It is never "
                                       "replaced automatically."));
        }
    }
    std::ifstream in(path);
    if (!in.is_open()) {
        return; // No file yet
    }

    nlohmann::ordered_json root;
    try {
        in >> root;
    } catch (const std::exception&) {
        QMessageBox::warning(
            this, tr("Parse error"),
            tr("The existing host_overrides.json could not be parsed. Starting with an "
               "empty list; saving will overwrite the file."));
        m_table->setRowCount(0);
        return;
    }
    if (!root.is_object()) {
        QMessageBox::warning(this, tr("Format error"),
                             tr("host_overrides.json must be a JSON object. Starting empty."));
        return;
    }

    for (const OverrideEntry& entry : ParseOverridesJson(root)) {
        AddRow(entry.match, entry.target, entry.enabled);
    }
}

bool HostOverridesDialog::SaveToDisk() {
    QStringList problems;
    nlohmann::ordered_json root = BuildOverridesJson(m_table, problems);

    if (!problems.isEmpty()) {
        QMessageBox::warning(
            this, tr("Cannot save"),
            tr("Please fix the following before saving:\n\n%1").arg(problems.join('\n')));
        return false;
    }

    const std::filesystem::path path = CurrentPath();
    std::error_code ec;

    if (IsPerGame()) {
        const auto etag_path = EtagPathFor(path);
        if (root.empty()) {
            // An empty per-game file would hide the global list; remove it instead.
            RemoveIfExists(path);
            RemoveIfExists(etag_path);
            return true;
        }
        // Unchanged downloaded file: keep it (and its marker) as is.
        if (std::filesystem::exists(etag_path, ec)) {
            std::ifstream current(path);
            try {
                nlohmann::ordered_json on_disk;
                current >> on_disk;
                if (on_disk == root) {
                    return true;
                }
            } catch (const std::exception&) {
            }
            // Edited: drop the marker so the emulator treats it as the user's file.
            RemoveIfExists(etag_path);
        }
    }

    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) {
        QString p;
        Common::FS::PathToQString(p, path);
        QMessageBox::critical(this, tr("Save failed"),
                              tr("Could not open the file for writing:\n%1").arg(p));
        return false;
    }
    out << root.dump(4) << '\n';
    if (out.fail()) {
        QMessageBox::critical(this, tr("Save failed"), tr("An error occurred while writing."));
        return false;
    }
    return true;
}

void HostOverridesDialog::OnDownload() {
    bool overwrite = false;
    if (HasUserWrittenFile(m_title_id)) {
        if (QMessageBox::question(
                this, tr("Replace local overrides?"),
                tr("This game has host overrides you edited yourself. Replace them with the "
                   "version published on shad-net?")) != QMessageBox::Yes) {
            return;
        }
        overwrite = true;
    }

    m_download_btn->setEnabled(false);
    m_download_btn->setText(tr("Downloading..."));
    QPointer<HostOverridesDialog> self(this);
    DownloadForTitle(
        m_title_id, this,
        [self](const HostOverridesDownloadResult& result) {
            if (!self) {
                return;
            }
            self->m_download_btn->setEnabled(true);
            self->m_download_btn->setText(tr("&Download from shad-net"));
            switch (result.status) {
            case HostOverridesDownloadResult::Status::Downloaded:
                self->LoadFromDisk();
                QMessageBox::information(self, tr("Download complete"),
                                         tr("%1\n\nLoaded %2 entry/entries.")
                                             .arg(result.message)
                                             .arg(self->m_table->rowCount()));
                break;
            case HostOverridesDownloadResult::Status::NotPublished:
                self->LoadFromDisk();
                QMessageBox::information(self, tr("Nothing to download"), result.message);
                break;
            case HostOverridesDownloadResult::Status::Failed:
                QMessageBox::warning(self, tr("Download failed"), result.message);
                break;
            }
        },
        overwrite);
}
