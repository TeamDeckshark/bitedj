// Tests for tableview-related things
// Right now it's just testing the serialize-unserialize of the header state code.
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QtDebug>
#include "proto/headers.pb.h"
#include "widget/wlibrarytableview.h"
#include "widget/wtracktableviewheader.h"

namespace {

class TestLibraryTableView final : public WLibraryTableView {
  public:
    TestLibraryTableView()
            : WLibraryTableView(nullptr, UserSettingsPointer()) {
    }

    void onShow() override {
    }

    bool hasFocus() const override {
        return QWidget::hasFocus();
    }

  protected:
    QString getModelStateKey() const override {
        return QStringLiteral("test-model");
    }
};

} // namespace

class HeaderViewStateTest : public testing::Test {
};

TEST_F(HeaderViewStateTest, RoundTrip) {
    mixxx::library::HeaderViewState headerViewState_pb;
    mixxx::library::HeaderViewState::HeaderState* header_state_pb =
                headerViewState_pb.add_header_state();

    header_state_pb->set_hidden(true);
    header_state_pb->set_size(50);
    header_state_pb->set_logical_index(10);
    header_state_pb->set_visual_index(2);
    header_state_pb->set_column_name("MyCol");

    header_state_pb =
                headerViewState_pb.add_header_state();

    header_state_pb->set_hidden(false);
    header_state_pb->set_size(22);
    header_state_pb->set_logical_index(6);
    header_state_pb->set_visual_index(3);
    header_state_pb->set_column_name("MyOtherCol");

    headerViewState_pb.set_sort_indicator_shown(true);
    headerViewState_pb.set_sort_indicator_section(1);
    headerViewState_pb.set_sort_order(Qt::DescendingOrder);

    // Create a HeaderViewState based on the proto.
    HeaderViewState view_state(headerViewState_pb);

    // Get a serialized form of the state.
    QString saved_state = view_state.saveState();

    // Initialize a new state object with the saved state.
    HeaderViewState loaded_state(saved_state);

    // Compare the old saved state with the new one.
    ASSERT_EQ(saved_state, loaded_state.saveState());

    // Ensure that the serialization is not bullshit.
    ASSERT_NE("", saved_state);
}

TEST_F(HeaderViewStateTest, GoodHeaderState) {
    const QString kGoodSerializedProto("ChEIARAAGAEgACoHcHJldmlldwoICAAQSxgEIAE"
            "KEggBEAAYAiACKghjb3ZlcmFydAoICAEQABgDIAMKCAgBEAAYBSAECggIABBaGBYgB"
            "QoICAEQABgGIAYKCQgAEPwBGBcgBwoJCAAQvAEYByAICgkIABDmARgIIAkKCAgBEAA"
            "YCSAKCggIARAAGAogCwoJCAAQkwQYGSAMCggIARAAGAwgDQoICAEQABgNIA4KCAgBE"
            "AAYDiAPCggIARAAGBAgEAoICAAQVxgRIBEKCAgBEAAYEiASCggIABBGGBMgEwoICAA"
            "QMhgPIBQKCAgAEHUYCyAVCggIARAAGBQgFgoICAAQNxgVIBcKCAgBEAAYGCAYCggIA"
            "RAAGBogGQoICAEQABgbIBoKCAgBEAAYHCAbCggIARAAGAAgHAoICAEQABgdIB0KCAg"
            "BEAAYHiAeEAEYFiAB");

    HeaderViewState view_state(kGoodSerializedProto);
    ASSERT_TRUE(view_state.healthy());
    ASSERT_EQ(kGoodSerializedProto, view_state.saveState());
}

TEST_F(HeaderViewStateTest, BadHeaderState) {
    HeaderViewState view_state("BLAHBLAHBLAHBAD");
    ASSERT_FALSE(view_state.healthy());
}

// A drive re-plugged under the same name rebuilds the model behind the same
// proxy and state key. The saved selection must be rebuilt from row numbers:
// the proxy indexes taken before the reset point into freed mapping data.
TEST(LibraryTableViewStateTest, RestoreAfterProxyResetUsesFreshIndexes) {
    QStandardItemModel sourceModel(3, 2);
    QSortFilterProxyModel proxyModel;
    proxyModel.setSourceModel(&sourceModel);

    TestLibraryTableView tableView;
    tableView.setModel(&proxyModel);

    tableView.selectionModel()->select(proxyModel.index(1, 0),
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
    tableView.selectionModel()->setCurrentIndex(
            proxyModel.index(1, 1), QItemSelectionModel::NoUpdate);
    tableView.saveTrackModelState(&proxyModel, QStringLiteral("usb-device"));

    sourceModel.clear();
    sourceModel.setColumnCount(2);
    sourceModel.setRowCount(4);

    ASSERT_TRUE(tableView.restoreTrackModelState(
            &proxyModel, QStringLiteral("usb-device")));
    const QModelIndexList restoredRows = tableView.selectionModel()->selectedRows();
    ASSERT_EQ(1, restoredRows.size());
    EXPECT_EQ(1, restoredRows.first().row());
    EXPECT_EQ(&proxyModel, restoredRows.first().model());
    EXPECT_EQ(1, tableView.currentIndex().row());
    EXPECT_EQ(1, tableView.currentIndex().column());
}

// Selected rows past the end of a model that came back shorter are dropped,
// and the current row is clamped to the last one.
TEST(LibraryTableViewStateTest, RestoreIntoShorterModelSkipsMissingRows) {
    QStandardItemModel model(6, 2);
    TestLibraryTableView tableView;
    tableView.setModel(&model);

    QItemSelection selection(model.index(1, 0), model.index(1, 1));
    selection.select(model.index(4, 0), model.index(5, 1));
    tableView.selectionModel()->select(selection,
            QItemSelectionModel::Select | QItemSelectionModel::Rows);
    tableView.selectionModel()->setCurrentIndex(
            model.index(5, 0), QItemSelectionModel::NoUpdate);
    tableView.saveTrackModelState(&model, QStringLiteral("usb-device"));

    model.setRowCount(3);

    ASSERT_TRUE(tableView.restoreTrackModelState(&model, QStringLiteral("usb-device")));
    const QModelIndexList restoredRows = tableView.selectionModel()->selectedRows();
    ASSERT_EQ(1, restoredRows.size());
    EXPECT_EQ(1, restoredRows.first().row());
    EXPECT_EQ(2, tableView.currentIndex().row());
}
