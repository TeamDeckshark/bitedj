#include "widget/wanalysislibrarytableview.h"

#include "moc_wanalysislibrarytableview.cpp"

WAnalysisLibraryTableView::WAnalysisLibraryTableView(
        QWidget* parent,
        UserSettingsPointer pConfig,
        Library* pLibrary,
        double trackTableBackgroundColorOpacity)
        : WTrackTableView(parent,
                  pConfig,
                  pLibrary,
                  trackTableBackgroundColorOpacity) {
    // Tracks can't be dragged out of the analysis table either, a press and
    // drag scrolls the list instead.
    setDragDropMode(QAbstractItemView::NoDragDrop);
    setDragEnabled(false);
}

void WAnalysisLibraryTableView::onSearch(const QString& text) {
    Q_UNUSED(text);
}
