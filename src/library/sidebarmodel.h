#pragma once

#include <QAbstractItemModel>
#include <QList>
#include <QModelIndex>
#include <QVariant>

class LibraryFeature;
class QTimer;

class SidebarModel : public QAbstractItemModel {
    Q_OBJECT
  public:
    // Keep object tree functions from QObject accessible
    // for parented_ptr
    using QObject::parent;

    enum Roles {
        IconNameRole = Qt::UserRole + 1,
        DataRole,
    };
    Q_ENUM(Roles);

    explicit SidebarModel(
            QObject* parent = nullptr);
    ~SidebarModel() override = default;

    void addLibraryFeature(LibraryFeature* feature);
    QModelIndex getDefaultSelection();
    void setDefaultSelection(unsigned int index);
    void activateDefaultSelection();

    // Required for QAbstractItemModel
    QModelIndex index(int row, int column,
                      const QModelIndex& parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex& index) const override;
    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index,
                  int role = Qt::DisplayRole) const override;
    bool dropAccept(const QModelIndex& index, const QList<QUrl>& urls, QObject* pSource);
    bool dragMoveAccept(const QModelIndex& index, const QUrl& url);
    bool hasChildren(const QModelIndex& parent = QModelIndex()) const override;
    bool hasTrackTable(const QModelIndex& index) const;
    QModelIndex translateChildIndex(const QModelIndex& index) {
        return translateIndex(index, index.model());
    }
    QModelIndex getFeatureRootIndex(LibraryFeature* pFeature);

    void clear(const QModelIndex& index);
    void paste(const QModelIndex& index);
  public slots:
    // Bite DJ: dynamically show/hide a registered feature's root item.
    // Connected to LibraryFeature::requestSidebarVisibility. Showing inserts
    // the row at the feature's original registration position.
    void setFeatureVisible(LibraryFeature* pFeature, bool visible);
    void pressed(const QModelIndex& index);
    void clicked(const QModelIndex& index);
    void doubleClicked(const QModelIndex& index);
    void rightClicked(const QPoint& globalPos, const QModelIndex& index);
    void renameItem(const QModelIndex& index);
    void deleteItem(const QModelIndex& index);
    void slotFeatureSelect(LibraryFeature* pFeature, const QModelIndex& index = QModelIndex());

    // Slots for every single QAbstractItemModel signal
    // void slotColumnsAboutToBeInserted(const QModelIndex& parent, int start, int end);
    // void slotColumnsAboutToBeRemoved(const QModelIndex& parent, int start, int end);
    // void slotColumnsInserted(const QModelIndex& parent, int start, int end);
    // void slotColumnsRemoved(const QModelIndex& parent, int start, int end);
    void slotDataChanged(const QModelIndex& topLeft, const QModelIndex & bottomRight);
    //void slotHeaderDataChanged(Qt::Orientation orientation, int first, int last);
    // void slotLayoutAboutToBeChanged();
    // void slotLayoutChanged();
    // void slotModelAboutToBeReset();
    // void slotModelReset();
    void slotRowsAboutToBeInserted(const QModelIndex& parent, int start, int end);
    void slotRowsAboutToBeRemoved(const QModelIndex& parent, int start, int end);
    void slotRowsInserted(const QModelIndex& parent, int start, int end);
    void slotRowsRemoved(const QModelIndex& parent, int start, int end);
    void slotModelAboutToBeReset();
    void slotModelReset();
    void slotFeatureIsLoading(LibraryFeature*, bool selectFeature);
    void slotFeatureLoadingFinished(LibraryFeature*);

  signals:
    void selectIndex(const QModelIndex& index);

  private slots:
    void slotPressedUntilClickedTimeout();

  private:
    QModelIndex translateSourceIndex(const QModelIndex& parent);
    QModelIndex translateIndex(const QModelIndex& index, const QAbstractItemModel* model);
    void featureRenamed(LibraryFeature*);
    LibraryFeature* featureForModel(const QAbstractItemModel* pModel) const;
    bool shouldSuppressChildModelSignals(int* pSuppressionCounter);
    // Features currently visible in the sidebar, in registration order.
    // Everywhere a QModelIndex row of a top-level item is mapped to a
    // feature, it indexes into this list.
    QList<LibraryFeature*> m_sFeatures;
    // All registered features in registration order, including hidden ones.
    QList<LibraryFeature*> m_allFeatures;
    // Depth counters pairing suppressed begin*/end* forwarding of row
    // change signals coming from hidden features' child models.
    int m_suppressedInserts = 0;
    int m_suppressedRemoves = 0;
    unsigned int m_iDefaultSelectedIndex; /** Index of the item in the sidebar model to select at startup. */

    QTimer* const m_pressedUntilClickedTimer;
    QModelIndex m_pressedIndex;

    void startPressedUntilClickedTimer(const QModelIndex& pressedIndex);
    void stopPressedUntilClickedTimer();
};
