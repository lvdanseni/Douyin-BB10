import bb.cascades 1.1


Button {
    id: chip
    property int rid: 0

    minWidth: ui.du(65)
    topMargin: ui.du(5)
    bottomMargin: ui.du(5)
    leftMargin: ui.du(5)
    rightMargin: ui.du(5)

    onClicked: {
        regionModel.setRegion(rid);
        regionModel.load();
    }
}
