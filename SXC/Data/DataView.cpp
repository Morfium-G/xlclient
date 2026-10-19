#include "SXC/Data/Data.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "SXC/ThirdParty/imgui/imgui.h"

namespace {
    using namespace SXC::Data;

    std::string s_selected;
    char s_filter[128];
    std::string s_saveError;

    // One cell as text: named values by name, flags by their bits, foreign keys with the target's name.
    std::string FormatCell(Table& table, const Row& row, int index) {
        const Field& field = table.GetSchema().FieldAt(index);
        char buffer[128];
        std::string text;

        for (uint32_t e = 0; e < field.count; e++) {
            if (e > 0) {
                text += ", ";
            }

            if (field.IsString()) {
                const std::string& value = row.String(index, e);
                const size_t newline = value.find('\n');
                text += newline == std::string::npos ? value : value.substr(0, newline) + " ...";
                continue;
            }

            double value;

            if (field.IsFloat()) {
                value = row.Float(index, e);
                snprintf(buffer, sizeof(buffer), "%g", value);
            } else if (field.type == FieldType::Uint || field.type == FieldType::Id) {
                value = row.Uint(index, e);
                snprintf(buffer, sizeof(buffer), "%u", row.Uint(index, e));
            } else if (field.type == FieldType::Flags || field.type == FieldType::Color) {
                value = row.Uint(index, e);
                snprintf(buffer, sizeof(buffer), field.colorFormat == ColorFormat::Argb ? "0x%08X" : "0x%06X", row.Uint(index, e));
            } else if (field.type == FieldType::Bool) {
                value = row.Uint(index, e);
                snprintf(buffer, sizeof(buffer), "%s", row.Bool(index, e) ? "yes" : "no");
            } else {
                value = row.Int(index, e);
                snprintf(buffer, sizeof(buffer), "%d", row.Int(index, e));
            }

            const char* name = field.NameOf(value);

            if (name && field.type == FieldType::Enum) {
                text += name;
                continue;
            }

            text += buffer;

            if (name) {
                text += std::string(" (") + name + ")";
            } else if (field.type == FieldType::Flags) {
                std::string names;
                const uint32_t bits = row.Uint(index, e);

                for (const NamedValue& bit : field.bits) {
                    if (bits & static_cast<uint32_t>(bit.value)) {
                        names += (names.empty() ? "" : ", ") + bit.name;
                    }
                }

                if (!names.empty()) {
                    text += " (" + names + ")";
                }
            } else if (field.type == FieldType::ForeignKey && value != 0.0 && Get(field.target.c_str())) {
                const Row targetRow = Get(field.target.c_str())->Find(row.Uint(index, e));

                if (!targetRow) {
                    text += " (missing)";
                } else if (!field.display.empty()) {
                    const std::string& display = targetRow.String(field.display.c_str());

                    if (!display.empty()) {
                        text += " " + display;
                    }
                }
            }
        }

        return text;
    }

    bool Contains(const std::string& text, const char* filter) {
        if (!*filter) {
            return true;
        }

        const size_t length = strlen(filter);

        for (size_t i = 0; i + length <= text.size(); i++) {
            if (_strnicmp(text.c_str() + i, filter, length) == 0) {
                return true;
            }
        }

        return false;
    }

    void DrawColorSwatch(const Field& field, const Row& row, int index) {
        ImVec4 color;

        if (field.colorFormat == ColorFormat::Float3) {
            color = ImVec4(row.Float(index, 0), row.Float(index, 1), row.Float(index, 2), 1.0f);
        } else {
            const uint32_t value = row.Uint(index);
            color = ImVec4(((value >> 16) & 0xFF) / 255.0f, ((value >> 8) & 0xFF) / 255.0f, (value & 0xFF) / 255.0f,
                           field.colorFormat == ColorFormat::Argb ? ((value >> 24) & 0xFF) / 255.0f : 1.0f);
        }

        const float size = ImGui::GetTextLineHeight();
        ImGui::ColorButton("##swatch", color, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_AlphaPreviewHalf, ImVec2(size, size));
        ImGui::SameLine();
    }

    void DrawTableList() {
        for (size_t i = 0; i < TableCount(); i++) {
            Table* table = TableAt(i);
            std::string label = table->Name() + (table->HasChanges() ? " *" : "");
            const bool problem = !table->Loaded() || table->ChangedOnDisk() || table->DuplicateIds();

            if (problem) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.3f, 1.0f));
            }

            if (ImGui::Selectable(label.c_str(), s_selected == table->Name())) {
                s_selected = table->Name();
                s_saveError.clear();
            }

            if (problem) {
                ImGui::PopStyleColor();
            }

            ImGui::SetItemTooltip("%s", table->Status().c_str());
        }
    }

    void DrawRows(Table& table) {
        const Schema& schema = table.GetSchema();
        std::vector<int> shown;

        for (size_t f = 0; f < schema.FieldCount(); f++) {
            if (!schema.FieldAt(f).hidden) {
                shown.push_back(static_cast<int>(f));
            }
        }

        // The rows that match the filter, as text in any shown field.
        std::vector<uint32_t> matches;

        for (uint32_t r = 0; r < table.RowCount(); r++) {
            if (!*s_filter) {
                matches.push_back(r);
                continue;
            }

            for (int f : shown) {
                if (Contains(FormatCell(table, table.At(r), f), s_filter)) {
                    matches.push_back(r);
                    break;
                }
            }
        }

        const ImGuiTableFlags flags = ImGuiTableFlags_ScrollX | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV
                                    | ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable | ImGuiTableFlags_Hideable;

        if (!ImGui::BeginTable("##rows", static_cast<int>(shown.size()), flags)) {
            return;
        }

        ImGui::TableSetupScrollFreeze(1, 1);

        for (int f : shown) {
            const Field& field = schema.FieldAt(f);
            ImGui::TableSetupColumn(field.label.c_str(), f == 0 ? ImGuiTableColumnFlags_NoHide : ImGuiTableColumnFlags_None);
        }

        // Headers with the field's tooltip.
        ImGui::TableNextRow(ImGuiTableRowFlags_Headers);

        for (size_t c = 0; c < shown.size(); c++) {
            const Field& field = schema.FieldAt(shown[c]);
            ImGui::TableSetColumnIndex(static_cast<int>(c));
            ImGui::TableHeader(field.label.c_str());

            if (ImGui::IsItemHovered()) {
                std::string tip = schema.ColumnName(shown[c], 0);

                if (field.count > 1) {
                    tip = field.name + " (" + std::to_string(field.count) + " columns)";
                }

                if (!field.tooltip.empty()) {
                    tip += "\n" + field.tooltip;
                }

                ImGui::SetTooltip("%s", tip.c_str());
            }
        }

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(matches.size()));

        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; i++) {
                const Row row = table.At(matches[i]);
                ImGui::TableNextRow();
                ImGui::PushID(static_cast<int>(matches[i]));

                for (size_t c = 0; c < shown.size(); c++) {
                    const Field& field = schema.FieldAt(shown[c]);
                    ImGui::TableSetColumnIndex(static_cast<int>(c));
                    ImGui::PushID(static_cast<int>(c));

                    if (field.type == FieldType::Color) {
                        DrawColorSwatch(field, row, shown[c]);
                    }

                    ImGui::TextUnformatted(FormatCell(table, row, shown[c]).c_str());
                    ImGui::PopID();
                }

                ImGui::PopID();
            }
        }

        ImGui::EndTable();
    }

    void DrawTable(Table& table) {
        const Schema& schema = table.GetSchema();
        ImGui::TextUnformatted(table.Name().c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", schema.FilePath().c_str());

        if (!schema.GetDescription().empty()) {
            ImGui::TextWrapped("%s", schema.GetDescription().c_str());
        }

        ImGui::TextDisabled("%s", table.Status().c_str());

        if (!schema.HotReload()) {
            ImGui::TextDisabled("Not reloaded when the file changes; use Reload.");
        }

        if (table.DuplicateIds()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%u rows share an id with another row", table.DuplicateIds());
        }

        if (table.ChangedOnDisk()) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "The file changed on disk, and this table has unsaved edits.");
        }

        if (ImGui::Button(table.HasChanges() ? "Reload (drop edits)" : "Reload")) {
            table.Reload();
            s_saveError.clear();
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(!table.HasChanges());

        if (ImGui::Button("Save")) {
            s_saveError.clear();
            table.Save(&s_saveError);
        }

        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", "filter rows", s_filter, sizeof(s_filter));

        if (!s_saveError.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", s_saveError.c_str());
        }

        DrawRows(table);
    }
}

namespace SXC::Data {
    void DrawBrowser() {
        if (TableCount() == 0) {
            ImGui::TextDisabled("no tables registered");
            return;
        }

        ImGui::BeginChild("##tables", ImVec2(ImGui::GetFontSize() * 14.0f, 0.0f), ImGuiChildFlags_ResizeX | ImGuiChildFlags_Borders);
        DrawTableList();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("##table");
        Table* table = Get(s_selected.c_str());

        if (table) {
            DrawTable(*table);
        } else {
            ImGui::TextDisabled("pick a table");
        }

        ImGui::EndChild();
    }
}
