import SwiftUI

struct InstallerReviewView: View {
    @ObservedObject var model: SwitchInstallerModel
    @ScaledMetric private var iconSize = 64

    var body: some View {
        List {
            if let package = model.package {
                Section {
                    summary(package)
                        .listRowSeparator(.hidden, edges: .bottom)
                    LabeledContent("Identifier") {
                        Text(package.info.identifier)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .listRowSeparator(.hidden, edges: .top)
                    LabeledContent(
                        "Installed size",
                        value: ByteCountFormatter.string(
                            fromByteCount: Int64(package.info.expandedSize), countStyle: .file
                        )
                    )
                    LabeledContent("Source") {
                        Text(package.source).fixedSize(horizontal: false, vertical: true)
                    }
                } footer: {
                    Text(
                        package.isUpdate
                            ? """
                            This updates the installed app and keeps its data. Only install apps from sources you trust.
                            """
                            : "Only install apps from sources you trust."
                    )
                }
                .listSectionSeparator(.hidden)
            }
            InstallerStatusView(model: model)
                .listSectionSeparator(.hidden)
        }
        .listStyle(.plain)
        .navigationTitle(model.package?.isUpdate == true ? "Update App" : "Review App")
        .navigationBarTitleDisplayMode(.inline)
        .navigationBarBackButtonHidden(model.phase == .installing)
    }

    private func summary(_ package: InstallerPackage) -> some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 16) {
                icon(package)
                identity(package)
            }
            VStack(alignment: .leading, spacing: 12) {
                icon(package)
                identity(package)
            }
        }
        .padding(.vertical, 8)
        .accessibilityElement(children: .combine)
    }

    @ViewBuilder
    private func icon(_ package: InstallerPackage) -> some View {
        if let image = package.icon {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
                .frame(width: iconSize, height: iconSize)
                .clipShape(RoundedRectangle(cornerRadius: iconSize * 0.22))
                .accessibilityHidden(true)
        } else {
            Image(systemName: "shippingbox")
                .font(.largeTitle)
                .frame(width: iconSize, height: iconSize)
                .foregroundStyle(.secondary)
                .accessibilityHidden(true)
        }
    }

    private func identity(_ package: InstallerPackage) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(package.info.name).font(.headline).textSelection(.enabled)
            Text("Version \(package.info.version)").font(.subheadline).foregroundStyle(.secondary)
        }
        .fixedSize(horizontal: false, vertical: true)
    }
}
