#include "AnkiExportJob.h"

#include <algorithm> // For std::min
#include <memory>   // for unique_ptr
#include <utility>  // for move, pair
#include <sstream>  // for stringstream
#include <zip.h>    // for zip operations
#include <sqlite3.h> // For SQLite database operations
#include <nlohmann/json.hpp> // For JSON operations (media.json)

#include <gtk/gtk.h>  // for GTK_WINDOW

#include "control/Control.h"         // for Control
#include "gui/MainWindow.h"          // for MainWindow
#include "model/Document.h"          // for Document
#include "util/PathUtil.h"           // for clearExtensions
#include "util/PopupWindowWrapper.h" // for PopupWindowWrapper
#include "util/Util.h"               // for execInUiThread, getDesktopFolder, DPI_NORMALIZATION_FACTOR
#include "util/XojMsgBox.h"          // for XojMsgBox
#include "util/i18n.h"               // for _, FS, _F
#include "util/glib_casts.h"         // for char_cast
#include "model/PageRef.h"           // for ConstPageRef
#include "model/XojPage.h"           // for XojPage
#include "view/DocumentView.h"       // for DocumentView
#include "view/background/BackgroundView.h" // for xoj::view::BackgroundFlags

#include "ImageExport.h" // for ImageExport, EXPORT_GR...
#include "XournalScheduler.h"
#include <chrono> // For std::chrono
#include <iomanip> // For std::put_time
#include <sys/stat.h> // For file stat operations
#include <random> // For random GUID generation
#include <locale> // For std::use_facet
#include <fstream> // For std::ofstream
using nlohmann::json;

// Helper to execute SQLite statements
static int sqlite_exec(sqlite3* db, const std::string& sql) {
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        std::string error = "SQL error: ";
        error += errMsg;
        sqlite3_free(errMsg);
        throw std::runtime_error(error);
    }
    return rc;
}

// Function to generate a random 8-character GUID
static std::string generate_guid() {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";
    std::string guid(8, ' ');
    std::random_device rd;
    std::mt19937 generator(rd());
    std::uniform_int_distribution<> distribution(0, sizeof(alphanum) - 2);
    for (int i = 0; i < 8; ++i) {
        guid[i] = alphanum[distribution(generator)];
    }
    return guid;
}


AnkiExportJob::AnkiExportJob(Control* control): BaseExportJob(control, _("Export to Anki Package (.apkg)")) {
    // Initialize export range to export all pages by default
    Document* doc = control->getDocument();
    doc->lock();
    size_t pageCount = doc->getPageCount();
    doc->unlock();

    if (pageCount > 0) {
        // Export all pages: from page 0 to pageCount-1
        exportRange = {{0, pageCount - 1}};
    }
    // If pageCount is 0, exportRange remains empty, which is handled properly in ImageExport
}

AnkiExportJob::~AnkiExportJob() = default;

void AnkiExportJob::run() {
    exportGraphics();
    if (!lastError.empty()) return;

    createAnkiDb();
    if (!lastError.empty()) return;

    createMediaJson();
    if (!lastError.empty()) return;

    createZipArchive();
}

void AnkiExportJob::addFilterToDialog(GtkFileChooser* dialog) {
    // File filter for Anki apkg files - only show .apkg files
    GtkFileFilter* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Anki Deck Package (*.apkg)");
    gtk_file_filter_add_pattern(filter, "*.apkg");
    gtk_file_chooser_add_filter(dialog, filter);

    // Also add a filter to show all files
    GtkFileFilter* allFilter = gtk_file_filter_new();
    gtk_file_filter_set_name(allFilter, "All Files");
    gtk_file_filter_add_pattern(allFilter, "*");
    gtk_file_chooser_add_filter(dialog, allFilter);

    // Set the .apkg filter as the default
    gtk_file_chooser_set_filter(dialog, filter);
}

void AnkiExportJob::setExtensionFromFilter(fs::path& file, const char* filterName) const {
    Util::clearExtensions(file, ".apkg");
    file += ".apkg";
}

// Function to show a dialog for deck name input
static std::string showDeckNameDialog(GtkWindow* parentWindow) {
    GtkDialog* dialog = GTK_DIALOG(gtk_dialog_new_with_buttons("Deck Name", parentWindow,
                                                                GTK_DIALOG_MODAL,
                                                                _("Cancel"), GTK_RESPONSE_CANCEL,
                                                                _("OK"), GTK_RESPONSE_OK,
                                                                nullptr));

    GtkWidget* contentArea = gtk_dialog_get_content_area(dialog);

    GtkWidget* label = gtk_label_new("Enter deck name:");
    gtk_container_add(GTK_CONTAINER(contentArea), label);

    GtkWidget* entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry), "Default");  // Default deck name
    gtk_entry_set_activates_default(GTK_ENTRY(entry), true);
    gtk_container_add(GTK_CONTAINER(contentArea), entry);

    gtk_widget_show_all(GTK_WIDGET(contentArea));

    std::string result = "Default";
    if (gtk_dialog_run(dialog) == GTK_RESPONSE_OK) {
        const char* text = gtk_entry_get_text(GTK_ENTRY(entry));
        g_debug("AnkiExportJob: Dialog returned OK, text input: '%s'", text ? text : "(null)");
        if (text && strlen(text) > 0) {
            result = std::string(text);
            g_debug("AnkiExportJob: Using user input for deck name: '%s'", result.c_str());
        } else {
            g_debug("AnkiExportJob: Using default deck name: 'Default'");
        }
    } else {
        g_debug("AnkiExportJob: Dialog was cancelled, using default deck name: 'Default'");
    }

    gtk_widget_destroy(GTK_WIDGET(dialog));
    return result;
}

void AnkiExportJob::showDialogAndRun() {
    auto onFileSelected = [job = this]() {
        Util::execInUiThread([job]() {
            // Show deck name dialog first
            std::string userDeckName = showDeckNameDialog(job->control->getGtkWindow());
            g_debug("AnkiExportJob: User entered deck name: '%s'", userDeckName.c_str());
            job->deckName = userDeckName;
            g_debug("AnkiExportJob: deckName member set to: '%s'", job->deckName.c_str());

            job->control->block(_("Exporting to Anki..."));

            if (job->filepath.empty()) {
                job->lastError = FS(_F("No destination file selected."));
                job->control->unblock();
                job->unref();
                return;
            }

            // Create a unique temp directory name based on document name and timestamp
            std::string documentName = job->control->getDocument()->getFilepath().stem().string();
            if (documentName.empty()) {
                documentName = "Untitled";
            }

            auto now = std::chrono::system_clock::now();
            auto in_time_t = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << "AnkiExport_" << documentName << "_" << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S");
            fs::path tempPath = fs::temp_directory_path() / ss.str();

            try {
                fs::create_directory(tempPath);
                job->tempPath = tempPath; // Store the temp path for later use
                job->control->getScheduler()->addJob(job, JOB_PRIORITY_NONE);
            } catch (const fs::filesystem_error& e) {
                job->lastError = FS(_F("Failed to create temporary directory: {1}") % e.what());
                job->control->unblock();
                job->unref();
            }
        });
    };

    auto onCancel = [job = this]() {
        job->control->unblock();
        job->unref();
    };

    // Use BaseExportJob's file chooser, but configured for apkg file selection
    BaseExportJob::showFileChooser(std::move(onFileSelected), std::move(onCancel));
}

void AnkiExportJob::renderAndSavePageHalf(size_t pageId, size_t pageNo, double zoomRatio, DocumentView& view,
                                          const fs::path& outputFilePath, double clipY, double clipHeight) {
    Document* doc = control->getDocument();
    doc->lock();
    ConstPageRef page = doc->getPage(pageId);
    doc->unlock();

    double pageWidth = page->getWidth();

    // Create a Cairo surface for the clipped portion
    cairo_surface_t* surface =
        cairo_image_surface_create(CAIRO_FORMAT_ARGB32, (int)std::round(pageWidth * zoomRatio),
                                   (int)std::round(clipHeight * zoomRatio));
    cairo_t* cr = cairo_create(surface);

    cairo_status_t state = cairo_surface_status(surface);
    if (state != CAIRO_STATUS_SUCCESS) {
        lastError = FS(_F("Error creating Cairo surface for page half: {1}") % cairo_status_to_string(state));
        cairo_destroy(cr);
        cairo_surface_destroy(surface);
        return;
    }

    // Scale and then translate to render the correct portion
    cairo_scale(cr, zoomRatio, zoomRatio);
    cairo_translate(cr, 0, -clipY); // Translate to bring the clipped section to the top of the new surface

    // Draw background if applicable
    xoj::view::BackgroundFlags flags;
    flags.showPDF = xoj::view::HIDE_PDF_BACKGROUND;
    flags.showImage = exportBackground == EXPORT_BACKGROUND_NONE ? xoj::view::HIDE_IMAGE_BACKGROUND :
                                                                   xoj::view::SHOW_IMAGE_BACKGROUND;
    flags.showRuling = exportBackground <= EXPORT_BACKGROUND_UNRULED ? xoj::view::HIDE_RULING_BACKGROUND :
                                                                       xoj::view::SHOW_RULING_BACKGROUND;

    view.drawPage(page, cr, true /* dont render eraseable */, flags);

    // Write to PNG
    state = cairo_surface_write_to_png(surface, char_cast(outputFilePath.u8string().c_str()));
    if (state != CAIRO_STATUS_SUCCESS) {
        lastError = FS(_F("Error saving page half to PNG: {1}") % cairo_status_to_string(state));
    }

    cairo_destroy(cr);
    cairo_surface_destroy(surface);
}

void AnkiExportJob::exportGraphics() {
    // Compute the zoomRatio once if using DPI as a PNG quality criterion
    double zoomRatio = 1.0;
    if ((pngQualityParameter.getQualityCriterion() == EXPORT_QUALITY_DPI)) {
        zoomRatio = ((double)pngQualityParameter.getValue()) / Util::DPI_NORMALIZATION_FACTOR;
    }

    // Cap scale factor to prevent high rendering times and massive files
    zoomRatio = std::min(zoomRatio, 1.25);

    DocumentView view;
    size_t currentMediaId = 0; // To generate unique Anki media IDs

    for (PageRangeEntry const& e : exportRange) {
        for (size_t pageId = e.first; pageId <= e.last; ++pageId) {
            if (!lastError.empty()) return; // Stop if an error occurred

            Document* doc = control->getDocument();
            doc->lock();
            ConstPageRef page = doc->getPage(pageId);
            doc->unlock();

            double pageHeight = page->getHeight(); 

            // Export Question Portion (Top 20%)
            fs::path questionFilename = tempPath / (std::to_string(currentMediaId++) + ".png");
            renderAndSavePageHalf(pageId, pageId + 1, zoomRatio, view, questionFilename, 0, pageHeight * 0.2);
            if (!lastError.empty()) return;
            mediaFiles.emplace_back(std::to_string(currentMediaId - 1), questionFilename);

            // Export Answer Portion (Bottom 80%)
            fs::path answerFilename = tempPath / (std::to_string(currentMediaId++) + ".png");
            renderAndSavePageHalf(pageId, pageId + 1, zoomRatio, view, answerFilename, pageHeight * 0.2, pageHeight * 0.8);
            if (!lastError.empty()) return;
            mediaFiles.emplace_back(std::to_string(currentMediaId - 1), answerFilename);
        }
    }
    foundAnkiFiles = !mediaFiles.empty(); // Set flag to indicate if files were created
}

void AnkiExportJob::createAnkiDb() {
    g_debug("AnkiExportJob: createAnkiDb called with deckName: '%s'", deckName.c_str());

    sqlite3* db;
    fs::path dbPath = tempPath / "collection.anki2";
    int rc = sqlite3_open(reinterpret_cast<const char*>(dbPath.u8string().c_str()), &db);
    if (rc != SQLITE_OK) {
        lastError = FS(_F("Failed to open Anki database: {1}") % sqlite3_errmsg(db));
        return;
    }

    try {
        // Create tables according to Anki database structure
        sqlite_exec(db, "CREATE TABLE col (id INTEGER PRIMARY KEY, crt INTEGER NOT NULL, mod INTEGER NOT NULL, scm INTEGER NOT NULL, ver INTEGER NOT NULL, dty INTEGER NOT NULL, usn INTEGER NOT NULL, ls INTEGER NOT NULL, conf TEXT NOT NULL, models TEXT NOT NULL, decks TEXT NOT NULL, dconf TEXT NOT NULL, tags TEXT NOT NULL);");
        sqlite_exec(db, "CREATE TABLE notes (id INTEGER PRIMARY KEY, guid TEXT NOT NULL, mid INTEGER NOT NULL, mod INTEGER NOT NULL, usn INTEGER NOT NULL, tags TEXT NOT NULL, flds TEXT NOT NULL, sfld INTEGER NOT NULL, csum INTEGER NOT NULL, flags INTEGER NOT NULL, data TEXT NOT NULL);");
        sqlite_exec(db, "CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER NOT NULL, did INTEGER NOT NULL, ord INTEGER NOT NULL, mod INTEGER NOT NULL, usn INTEGER NOT NULL, type INTEGER NOT NULL, queue INTEGER NOT NULL, due INTEGER NOT NULL, ivl INTEGER NOT NULL, factor INTEGER NOT NULL, reps INTEGER NOT NULL, lapses INTEGER NOT NULL, left INTEGER NOT NULL, odue INTEGER NOT NULL, odid INTEGER NOT NULL, flags INTEGER NOT NULL, data TEXT NOT NULL);");
        sqlite_exec(db, "CREATE TABLE revlog (id INTEGER PRIMARY KEY, cid INTEGER NOT NULL, usn INTEGER NOT NULL, ease INTEGER NOT NULL, ivl INTEGER NOT NULL, lastIvl INTEGER NOT NULL, factor INTEGER NOT NULL, time INTEGER NOT NULL, type INTEGER NOT NULL);");
        sqlite_exec(db, "CREATE TABLE graves (usn INTEGER NOT NULL, oid INTEGER NOT NULL, type INTEGER NOT NULL);");

        // Create indexes for better performance
        sqlite_exec(db, "CREATE INDEX ix_notes_usn ON notes (usn);");
        sqlite_exec(db, "CREATE INDEX ix_notes_csum ON notes (csum);");
        sqlite_exec(db, "CREATE INDEX ix_cards_usn ON cards (usn);");
        sqlite_exec(db, "CREATE INDEX ix_cards_nid ON cards (nid);");
        sqlite_exec(db, "CREATE INDEX ix_cards_sched ON cards (did, queue, due);");
        sqlite_exec(db, "CREATE INDEX ix_revlog_usn ON revlog (usn);");
        sqlite_exec(db, "CREATE INDEX ix_revlog_cid ON revlog (cid);");

        long long current_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch()).count();
        long long current_time_s = current_time_ms / 1000;
        long long creation_time_s = current_time_s - 1; // Set creation time slightly before mod time

        // Generate dynamic Deck ID
        long long deck_id = current_time_ms;
        std::string deck_id_str = std::to_string(deck_id);

        // Create the collection configuration JSON
        json conf_json = {
            {"activeDecks", json::array({deck_id})},
            {"curDeck", deck_id},
            {"newBury", true},
            {"newSpread", 0},
            {"collapseTime", 1200},
            {"timeLim", 0},
            {"estTimes", true},
            {"dueCounts", true},
            {"curModel", current_time_ms},
            {"nextPos", 1},
            {"sortType", "noteFld"},
            {"sortBackwards", false},
            {"addToCur", true},
            {"dayLearnFirst", false},
            {"schedVer", 2},
            {"mod", current_time_s},
            {"usn", -1},
            {"scm", current_time_ms},
            {"dty", 0},
            {"ls", 0},
            {"crt", creation_time_s}
        };

        // Create the models JSON
        json models_json;
        json fields_array = json::array();
        fields_array.push_back({
            {"name", "Front"},
            {"ord", 0},
            {"sticky", false},
            {"rtl", false},
            {"font", "Arial"},
            {"size", 20},
            {"media", json::array()}
        });
        fields_array.push_back({
            {"name", "Back"},
            {"ord", 1},
            {"sticky", false},
            {"rtl", false},
            {"font", "Arial"},
            {"size", 20},
            {"media", json::array()}
        });

        json tmpls_array = json::array();
        tmpls_array.push_back({
            {"name", "Card 1"},
            {"ord", 0},
            {"qfmt", "{{Front}}"},
            {"afmt", "{{Front}}<hr id=answer>{{Back}}"},
            {"bqfmt", ""},
            {"bafmt", ""},
            {"did", nullptr}
        });
        tmpls_array.push_back({
            {"name", "Card 2"},
            {"ord", 1},
            {"qfmt", "{{Back}}"},
            {"afmt", "{{Back}}<hr id=answer>{{Front}}"},
            {"bqfmt", ""},
            {"bafmt", ""},
            {"did", nullptr}
        });

        json model_item = {
            {"id", std::to_string(current_time_ms)},
            {"name", "Xournalpp Image Note"},
            {"type", 0}, // 0 for standard, 1 for cloze
            {"mod", current_time_s},
            {"usn", -1},
            {"sortf", 0},
            {"did", deck_id}, // Dynamic deck ID
            {"tmpls", tmpls_array},
            {"flds", fields_array},
            {"css", ".card { font-family: arial; font-size: 20px; text-align: center; color: black; background-color: white; }\n\n.night_mode .card { color: white; background-color: #212121; }\n"},
            {"latexPre", "\\documentclass[12pt]{article}\n\\special{papersize=3in,5in}\n\\usepackage{amssymb,amsmath}\n\\pagestyle{empty}\n\\begin{document}\n"},
            {"latexPost", "\\end{document}"},
            {"req", json::array({json::array({0, "all", json::array({0})}), json::array({1, "all", json::array({1})})})},
            {"tags", json::array()}
        };
        models_json[std::to_string(current_time_ms)] = model_item;

        // Create the decks JSON
        g_debug("AnkiExportJob: Creating deck JSON with name: '%s'", deckName.c_str());
        json decks_json;
        json deck_item = {
            {"name", deckName},
            {"id", deck_id}, // Dynamic deck ID
            {"mod", current_time_s},
            {"usn", -1},
            {"type", "std"},
            {"desc", ""},
            {"dyn", 0}, // 0 for normal, 1 for filtered
            {"conf", 1},
            {"extendNew", 10},
            {"extendRev", 50},
            {"collapsed", false},
            {"browserCollapsed", false},
            {"newToday", json::array({0, 0})},
            {"revToday", json::array({0, 0})},
            {"lrnToday", json::array({0, 0})},
            {"timeToday", json::array({0, 0})},
            {"new", 0},
            {"rev", 0},
            {"lrn", 0},
            {"opts", json::object()}
        };
        decks_json[deck_id_str] = deck_item;

        // Create the deck configuration JSON
        json dconf_json;
        json dconf_item = {
            {"id", 1},
            {"name", "Default"},
            {"mod", current_time_s},
            {"usn", -1},
            {"type", "std"},
            {"new", {
                {"delays", json::array({1.0, 10.0})},
                {"ints", json::array({1, 4, 7})}, // default intervals
                {"initialFactor", 2500},
                {"separate", true},
                {"order", 1}, // 0=random, 1=due order
                {"perDay", 20}
            }},
            {"rev", {
                {"perDay", 200},
                {"ease4", 1.3},
                {"fuzz", 0.05},
                {"minSpace", 1},
                {"ivlFct", 1},
                {"maxIvl", 36500}
            }},
            {"lapse", {
                {"delays", json::array({10.0})},
                {"mult", 0},
                {"minInt", 1},
                {"leechFails", 8},
                {"leechAction", 0}, // 0=suspend, 1=tag only
                {"resched", true}
            }},
            {"timer", 0}, // 0=off, 1=on
            {"autoplay", true},
            {"replayq", true},
            {"maxTaken", 60}
        };
        dconf_json["1"] = dconf_item;

        // Insert the collection record
        std::string sql = "INSERT INTO col VALUES (1, " +
                         std::to_string(creation_time_s) + ", " +
                         std::to_string(current_time_s) + ", " +
                         std::to_string(current_time_ms) + ", 11, 0, -1, 0, " +
                         "'" + conf_json.dump() + "', " +
                         "'" + models_json.dump() + "', " +
                         "'" + decks_json.dump() + "', " +
                         "'" + dconf_json.dump() + "', " +
                         "'{}');";
        sqlite_exec(db, sql);

        // Generate notes and cards from media files
        long long note_id_base = current_time_ms;
        long long card_id_base = current_time_ms;

        for (size_t i = 0; i < mediaFiles.size(); i += 2) {
            if (!lastError.empty()) return;

            if (i + 1 >= mediaFiles.size()) {
                lastError = _("Mismatched media files - expected pairs");
                return;
            }

            fs::path question_filepath = mediaFiles[i].second;
            fs::path answer_filepath = mediaFiles[i+1].second;

            std::string question_filename = question_filepath.filename().string();
            std::string answer_filename = answer_filepath.filename().string();

            // Create note content
            std::string front_field = "<img src=\"" + question_filename + "\">";
            std::string back_field = "<img src=\"" + answer_filename + "\">";
            std::string flds = front_field + "\x1f" + back_field; // field separator

            long long note_id = note_id_base + (i/2);
            std::string guid = generate_guid();
            std::hash<std::string> hasher;
            size_t csum = hasher(front_field) % 0xffffff; // Anki uses 24-bit checksum

            // Insert note
            std::string note_sql = "INSERT INTO notes VALUES (" +
                                  std::to_string(note_id) + ", " +
                                  "'" + guid + "', " +
                                  std::to_string(current_time_ms) + ", " + // mid
                                  std::to_string(current_time_s) + ", " +
                                  "-1, " + // usn
                                  "'', " + // tags
                                  "'" + flds + "', " +
                                  "0, " + // sfld
                                  std::to_string(csum) + ", " +
                                  "0, " + // flags
                                  "'');"; // data
            sqlite_exec(db, note_sql);

            // Create cards for each note
            long long card1_id = card_id_base + i;
            std::string card1_sql = "INSERT INTO cards VALUES (" +
                                   std::to_string(card1_id) + ", " + // id
                                   std::to_string(note_id) + ", " + // nid
                                   std::to_string(deck_id) + ", " + // did (dynamic deck id)
                                   "0, " + // ord (template 0)
                                   std::to_string(current_time_s) + ", " + // mod
                                   "-1, " + // usn
                                   "0, " + // type
                                   "0, " + // queue
                                   "0, " + // due
                                   "0, " + // ivl
                                   "0, " + // factor
                                   "0, " + // reps
                                   "0, " + // lapses
                                   "0, " + // left
                                   "0, " + // odue
                                   "0, " + // odid
                                   "0, " + // flags
                                   "'');"; // data
            sqlite_exec(db, card1_sql);
        }
    } catch (const std::exception& e) {
        lastError = FS(_F("Anki DB error: {1}") % e.what());
    } catch (...) {
        lastError = _("Unknown Anki DB error");
    }

    sqlite3_close(db);
}

void AnkiExportJob::createMediaJson() {
    json media_json;
    for (const auto& entry : mediaFiles) {
        media_json[entry.first] = entry.second.filename().string();
    }

    fs::path mediaPath = tempPath / "media"; // No extension for media file
    std::ofstream o(reinterpret_cast<const char*>(mediaPath.u8string().c_str()));
    if (!o.is_open()) {
        lastError = FS(_F("Failed to create media JSON file: {1}") % mediaPath.u8string());
        return;
    }
    o << media_json.dump();
    o.close();
}

void AnkiExportJob::createZipArchive() {

    if (!foundAnkiFiles) {
        lastError = FS(_F("No Anki-related files found in temporary export directory: {1}") % tempPath.u8string());
        return;
    }

    // Create zip file
    int errorCode = 0;
    zip_t* zip = zip_open(char_cast(filepath.u8string().c_str()), ZIP_CREATE | ZIP_TRUNCATE, &errorCode);
    if (!zip) {
        lastError = FS(_F("Failed to create ZIP archive: error code {1}") % errorCode);
        return;
    }

    // Add all files from temp directory to the zip
    for (const auto& entry: fs::recursive_directory_iterator(tempPath)) {
        if (entry.is_regular_file()) {
            fs::path relativePath = entry.path().lexically_relative(tempPath);

            // Special handling for PNG files: they should be renamed to just their numeric ID in the archive
            std::string archivePath = relativePath.string();
            if (relativePath.extension() == ".png") {
                // If the filename is a number followed by .png, use just the number for the archive
                std::string stem = relativePath.stem().string(); // Get the part without extension
                // Check if stem is numeric to ensure it's a media file
                bool isNumeric = !stem.empty() && std::all_of(stem.begin(), stem.end(), ::isdigit);
                if (isNumeric) {
                    archivePath = stem;  // Use just the numeric part without extension
                }
            }

            zip_source_t* source = zip_source_file(zip, char_cast(entry.path().u8string().c_str()), 0, -1);
            if (!source) {
                lastError = FS(_F("Failed to create source for file in ZIP: {1}") % entry.path().u8string());
                zip_close(zip);
                return;
            }

            // Add with the potentially modified archive path
            if (zip_file_add(zip, archivePath.c_str(), source, ZIP_FL_ENC_UTF_8) < 0) {
                lastError = FS(_F("Failed to add file to ZIP: {1}") % entry.path().u8string());
                zip_source_free(source);
                zip_close(zip);
                return;
            }
        }
    }

    // Close the zip file (this actually writes it to disk)
    errorCode = zip_close(zip);
    if (errorCode) {
        lastError = FS(_F("Error occurred while closing ZIP archive: error code {1}") % errorCode);
    }
}

void AnkiExportJob::afterRun() {
    control->unblock();
    // The error message is handled by BaseExportJob::afterRun()
    // For success, BaseExportJob::afterRun() does not show a message, so we show it here.
    if (this->lastError.empty()) {
        XojMsgBox::showMessageToUser(control->getGtkWindow(),
                                   FS(_F("Successfully exported to Anki Package (.apkg): {1}") %
                                      reinterpret_cast<const char*>(this->filepath.u8string().c_str())),
                                   GTK_MESSAGE_INFO);
    }
}