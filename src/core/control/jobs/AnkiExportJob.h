/*
 * Xournal++
 *
 * Export to Anki
 *
 * @author Xournal++ Team
 * https://github.com/xournalpp/xournalpp
 *
 * @license GNU GPLv2 or later
 */

#pragma once

#include <string>  // for string

#include "BaseExportJob.h"  // for BaseExportJob, EXPORT_BACKGROUND_ALL
#include "ImageExport.h"    // for RasterImageQualityParameter, EXPORT_GRAP...
#include "util/ElementRange.h"  // for PageRangeVector
#include "filesystem.h"     // for path

class Control;

class AnkiExportJob: public BaseExportJob {
public:
    AnkiExportJob(Control* control);
    ~AnkiExportJob() override;

public:
    void run() override;
    void showDialogAndRun();

private:
    void exportGraphics();
    void createZipArchive();
    void createAnkiDb(); /**< Creates the Anki SQLite database (collection.anki2). */
    void createMediaJson(); /**< Creates the Anki media JSON file (media). */

    /**
     * @brief Renders and saves a specific portion of a page to an image file.
     * @param pageId The 0-based index of the page in the document.
     * @param pageNo The 1-based number of the page for naming conventions.
     * @param zoomRatio The zoom ratio to apply for rendering.
     * @param view The DocumentView used for drawing the page content.
     * @param outputFilePath The full path where the image will be saved.
     * @param clipY The Y-coordinate to start clipping from (in page coordinates).
     * @param clipHeight The height of the clipped region (in page coordinates).
     */
    void renderAndSavePageHalf(size_t pageId, size_t pageNo, double zoomRatio, DocumentView& view,
                               const fs::path& outputFilePath, double clipY, double clipHeight);

protected:
    void afterRun() override;
    void addFilterToDialog(GtkFileChooser* dialog) override;
    void setExtensionFromFilter(fs::path& p, const char* filterName) const override;

private:
    /**
     * The range of pages to export.
     */
    PageRangeVector exportRange;

    /**
     * @brief Quality parameters for PNG exports, such as DPI or target width/height.
     */
    RasterImageQualityParameter pngQualityParameter = RasterImageQualityParameter();

    /**
     * Specifies how the background (PDF, image, ruling) should be exported.
     */
    ExportBackgroundType exportBackground = EXPORT_BACKGROUND_ALL;

    std::string lastError;

    /**
     * Path to the temporary directory where intermediate files (images, Anki DB, media.json) are stored.
     */
    fs::path tempPath;

    /**
     * Stores a list of pairs, where each pair consists of the Anki-internal media ID (as a string)
     * and the filesystem path to the corresponding generated media file (e.g., PNG image).
     * This list is crucial for building the Anki database and media.json file.
     */
    std::vector<std::pair<std::string, fs::path>> mediaFiles;

    /**
     * Flag to indicate whether Anki-related files were created during export.
     */
    bool foundAnkiFiles = false;

    /**
     * The deck name to use for the exported Anki package.
     */
    std::string deckName = "Default";
};
