// This file is part of the Godot Orchestrator project.
//
// Copyright (c) 2023-present Vahera Studios LLC and its contributors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//		http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
#include "script_view.h"

#include "api/extension_db.h"
#include "common/callable_lambda.h"
#include "common/name_utils.h"
#include "common/scene_utils.h"
#include "editor/component_panels/component_panel.h"
#include "editor/component_panels/functions_panel.h"
#include "editor/component_panels/graphs_panel.h"
#include "editor/component_panels/macros_panel.h"
#include "editor/component_panels/signals_panel.h"
#include "editor/component_panels/variables_panel.h"
#include "editor/graph/graph_edit.h"
#include "editor/main_view.h"
#include "script/nodes/functions/call_script_function.h"
#include "script/nodes/functions/event.h"
#include "script/nodes/functions/function_entry.h"
#include "script/nodes/functions/function_result.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_mouse_button.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/margin_container.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/rich_text_label.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/scroll_container.hpp>
#include <godot_cpp/classes/style_box_flat.hpp>
#include <godot_cpp/classes/theme.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/templates/vector.hpp>

Ref<OScriptFunction> OrchestratorScriptView::_create_new_function(const String& p_name, bool p_add_return_node)
{
    ERR_FAIL_COND_V_MSG(_orchestration->has_graph(p_name), {}, "Script already has graph named " + p_name);

    Ref<OScriptGraph> graph = _orchestration->create_graph(p_name, OScriptGraph::GF_FUNCTION | OScriptGraph::GF_DEFAULT);
    ERR_FAIL_COND_V_MSG(!graph.is_valid(), {}, "Failed to create new function graph named " + p_name);

    MethodInfo mi;
    mi.name = p_name;
    mi.flags = METHOD_FLAG_NORMAL;
    mi.return_val.type = Variant::NIL;
    mi.return_val.hint = PROPERTY_HINT_NONE;
    mi.return_val.usage = PROPERTY_USAGE_DEFAULT;

    OScriptNodeInitContext context;
    context.method = mi;

    const Ref<OScriptNodeFunctionEntry> entry = graph->create_node<OScriptNodeFunctionEntry>(context);
    if (!entry.is_valid())
    {
        _orchestration->remove_graph(graph->get_graph_name());
        ERR_FAIL_V_MSG({}, "Failed to create function entry node for function " + p_name);
    }

    if (p_add_return_node)
    {
        const Vector2 position = entry->get_position() + Vector2(300, 0);
        const Ref<OScriptNodeFunctionResult> result = graph->create_node<OScriptNodeFunctionResult>(context, position);
        if (!result.is_valid())
            ERR_PRINT(vformat("Failed to spawn result node for function '%s'.", p_name));
    }

    _functions->update();

    return entry->get_function();
}

void OrchestratorScriptView::_resolve_node_set_connections(const Vector<Ref<OScriptNode>>& p_nodes, NodeSetConnections& r_connections)
{
    // Create a map of the nodes
    HashMap<int, Ref<OScriptNode>> node_map;
    for (const Ref<OScriptNode>& node : p_nodes)
    {
        node_map[node->get_id()] = node;

        const Vector<Ref<OScriptNodePin>> inputs = node->find_pins(PD_Input);
        for (const Ref<OScriptNodePin>& input : inputs)
        {
            for (const Ref<OScriptNodePin>& E : input->get_connections())
            {
                if (!p_nodes.has(E->get_owning_node()))
                {
                    if (input->is_execution())
                        r_connections.input_executions++;
                    else
                        r_connections.input_data++;
                }
            }
        }

        const Vector<Ref<OScriptNodePin>> outputs = node->find_pins(PD_Output);
        for (const Ref<OScriptNodePin>& output : outputs)
        {
            for (const Ref<OScriptNodePin>& E : output->get_connections())
            {
                if (!p_nodes.has(E->get_owning_node()))
                {
                    if (output->is_execution())
                        r_connections.output_executions++;
                    else
                        r_connections.output_data++;
                }
            }
        }
    }

    for (const OScriptConnection& E : _orchestration->get_connections())
    {
        if (node_map.has(E.from_node) && node_map.has(E.to_node))
            r_connections.connections.insert(E);

        if (!node_map.has(E.from_node) && node_map.has(E.to_node))
            r_connections.inputs.insert(E);

        if (node_map.has(E.from_node) && !node_map.has(E.to_node))
            r_connections.outputs.insert(E);
    }
}

Rect2 OrchestratorScriptView::_get_node_set_rect(const Vector<Ref<OScriptNode>>& p_nodes) const
{
    if (p_nodes.is_empty())
        return {};

    Rect2 area(p_nodes[0]->get_position(), Vector2());
    for (const Ref<OScriptNode>& E : p_nodes)
        area.expand_to(E->get_position());
    return area;
}

void OrchestratorScriptView::_collapse_selected_to_function(OrchestratorGraphEdit* p_graph)
{
    Vector<Ref<OScriptNode>> selected = p_graph->get_selected_script_nodes();
    if (selected.is_empty())
        return;

    for (const Ref<OScriptNode>& node : selected)
    {
        if (!node->can_duplicate())
        {
            ERR_FAIL_MSG("Cannot collapse because node " + itos(node->get_id()) + " cannot be duplicated.");
        }
    }

    // Capture connections based on the selected nodes
    NodeSetConnections connections;
    _resolve_node_set_connections(selected, connections);

    ERR_FAIL_COND_EDMSG(connections.input_executions > 1, "Cannot collapse to function with more than one external input execution wire.");
    ERR_FAIL_COND_EDMSG(connections.output_executions > 1, "Cannot collapse to function with more than one external output execution wire.");
    ERR_FAIL_COND_EDMSG(connections.outputs.size() > 2, "Cannot output more than one execution and one data pin.");

    const String new_function_name = NameUtils::create_unique_name("NewFunction", _orchestration->get_function_names());
    Ref<OScriptFunction> function = _create_new_function(new_function_name, true);
    if (!function.is_valid())
        return;

    const Ref<OScriptGraph> source_graph = p_graph->get_owning_graph();
    const Ref<OScriptGraph> target_graph = function->get_function_graph();

    // Calculate the area of the original nodes
    const Rect2 area = _get_node_set_rect(selected);

    // Before we move the nodes, we need to severe their connections to the outside world
    for (const OScriptConnection& E : connections.inputs)
        source_graph->unlink(E.from_node, E.from_port, E.to_node, E.to_port);
    for (const OScriptConnection& E : connections.outputs)
        source_graph->unlink(E.from_node, E.from_port, E.to_node, E.to_port);

    // Move node between the two graphs
    for (const Ref<OScriptNode>& E : selected)
        source_graph->move_node_to(E, target_graph);

    OScriptNodeInitContext context;
    context.method = function->get_method_info();

    Ref<OScriptNode> call_node = source_graph->create_node<OScriptNodeCallScriptFunction>(context, area.get_center());

    const Ref<OScriptNodeFunctionEntry> entry = _orchestration->get_node(function->get_owning_node_id());
    const Ref<OScriptNodeFunctionResult> result = function->get_return_node();

    int input_index = 1;
    int call_input_index = 1;
    bool input_execution_wired = false;
    bool call_execution_wired = false;
    bool entry_positioned = false;
    for (const OScriptConnection& E : connections.inputs)
    {
        // The exterior node connected to the selected node
        const Ref<OScriptNode> source = _orchestration->get_node(E.from_node);
        const Ref<OScriptNodePin> source_pin = source->find_pins(PD_Output)[E.from_port];
        if (source_pin->is_execution() && !call_execution_wired)
        {
            source_graph->link(E.from_node, E.from_port, call_node->get_id(), 0);
            call_execution_wired = true;
        }
        else if (!source_pin->is_execution())
        {
            source_graph->link(E.from_node, E.from_port, call_node->get_id(), call_input_index++);
        }

        // The selected node that is connected from the outside
        const Ref<OScriptNode> target = _orchestration->get_node(E.to_node);
        const Ref<OScriptNodePin> target_pin = target->find_pins(PD_Input)[E.to_port];

        if (!entry_positioned)
        {
            entry->set_position(target->get_position() - Vector2(250, 0));
            entry->emit_changed();
            entry_positioned = true;
        }

        if (!target_pin->is_execution())
        {
            const size_t size = function->get_argument_count() + 1;
            function->resize_argument_list(size);
            function->set_argument_name(size - 1, target_pin->get_pin_name());
            function->set_argument_type(size - 1, target_pin->get_type());

            // Wire entry data output to this connection
            target_graph->link(entry->get_id(), input_index++, E.to_node, E.to_port);
        }
        else if (!input_execution_wired)
        {
            // Wire entry execution output to this connection
            target_graph->link(entry->get_id(), 0, E.to_node, E.to_port);
            input_execution_wired = true;
        }
    }

    if(result.is_valid())
    {
        bool output_execution_wired = false;
        bool output_data_wired = false;
        bool positioned = false;
        for (const OScriptConnection& E : connections.outputs)
        {
            // The selected node that is connected from the ouside world
            const Ref<OScriptNode> source = _orchestration->get_node(E.from_node);
            const Ref<OScriptNodePin> source_pin = source->find_pins(PD_Output)[E.from_port];

            if (!positioned)
            {
                result->set_position(source->get_position() + Vector2(250, 0));
                result->emit_changed();
                positioned = true;
            }

            if (source_pin->is_execution() && !output_execution_wired) // Connect execution
            {
                target_graph->link(E.from_node, E.from_port, result->get_id(), 0);
                output_execution_wired = true;
            }
            else if (!source_pin->is_execution() && !output_data_wired) // Connect data
            {
                function->set_has_return_value(true);
                function->set_return_type(source_pin->get_type());

                target_graph->link(E.from_node, E.from_port, result->get_id(), 1);
                output_data_wired = true;
            }
        }

        const Ref<OScriptNodePin> result_exec = result->find_pin(0, PD_Input);
        if (result_exec.is_valid() && !result_exec->has_any_connections())
        {
            const Ref<OScriptNodePin> entry_exec = entry->find_pin(0, PD_Output);
            if (entry_exec.is_valid() && !entry_exec->has_any_connections())
            {
                entry_exec->link(result_exec);
                if (entry->find_pins(PD_Output).size() == 1)
                {
                    entry->set_position(result->get_position() - Vector2(250, 0));
                    entry->emit_changed();
                }
            }
        }
    }

    // Wire call node
    int call_output_index = 1;
    call_execution_wired = false;
    for (const OScriptConnection& E : connections.outputs)
    {
        // The exterior node connected to the selected node
        const Ref<OScriptNode> target = _orchestration->get_node(E.to_node);
        const Ref<OScriptNodePin> target_pin = target->find_pins(PD_Input)[E.to_port];
        if (target_pin->is_execution() && !call_execution_wired)
        {
            source_graph->link(call_node->get_id(), 0, E.to_node, E.to_port);
            call_execution_wired = true;
        }
        else if (!target_pin->is_execution())
        {
            source_graph->link(call_node->get_id(), call_output_index++, E.to_node, E.to_port);
        }
    }

    call_node->emit_changed();

    _functions->find_and_edit(function->get_function_name());
}

void OrchestratorScriptView::_expand_node(int p_node_id, OrchestratorGraphEdit* p_graph)
{
    const Ref<OScriptNodeCallScriptFunction> call_node = _orchestration->get_node(p_node_id);
    if (!call_node.is_valid())
        return;

    const Ref<OScriptFunction> function = call_node->get_function();
    if (!function.is_valid())
        return;

    const Ref<OScriptGraph> function_graph = function->get_function_graph();

    Vector<Ref<OScriptNode>> selected;
    for (const Ref<OScriptNode>& graph_node : function_graph->get_nodes())
    {
        Ref<OScriptNodeFunctionEntry> entry = graph_node;
        Ref<OScriptNodeFunctionResult> result = graph_node;
        if (!entry.is_valid() && !result.is_valid() && graph_node->can_duplicate())
            selected.push_back(graph_node);
    }

    if (selected.is_empty())
        return;

    const Rect2 area = _get_node_set_rect(selected);
    const Vector2 pos_delta = call_node->get_position() - area.get_center();

    HashMap<int, int> node_remap;
    for (const Ref<OScriptNode>& node : selected)
    {
        const Ref<OScriptNode> duplicate = p_graph->get_owning_graph()->duplicate_node(
            node->get_id(),
            pos_delta,
            true);

        // Record mapping between old and new nodes
        node_remap[node->get_id()] = duplicate->get_id();
    }

    // Record connections
    NodeSetConnections connections;
    _resolve_node_set_connections(selected, connections);

    // Reapply connections among pasted nodes
    for (const OScriptConnection& E : connections.connections)
        p_graph->get_owning_graph()->link(node_remap[E.from_node], E.from_port, node_remap[E.to_node], E.to_port);

    // Remove call node
    p_graph->get_orchestration()->remove_node(call_node->get_id());
}

void OrchestratorScriptView::_meta_clicked(const Variant& p_value)
{
    _build_errors_dialog->hide();

    const Dictionary value = JSON::parse_string(String(p_value));
    if (value.has("goto_node"))
        goto_node(String(value["goto_node"]).to_int());
}

bool OrchestratorScriptView::is_same_script(const Ref<OScript>& p_script) const
{
    const Ref<OScript> script = _resource;
    return script.is_valid() && p_script == script;
}

void OrchestratorScriptView::goto_node(int p_node_id)
{
    Ref<OScriptNode> node = _orchestration->get_node(p_node_id);
    if (node.is_valid())
    {
        for (const Ref<OScriptGraph>& graph : _orchestration->get_graphs())
        {
            if (graph->has_node(p_node_id))
            {
                OrchestratorGraphEdit* ed_graph = _get_or_create_tab(graph->get_graph_name(), true, true);
                if (ed_graph)
                {
                    ed_graph->focus_node(p_node_id);
                    break;
                }
            }
        }
    }
}

void OrchestratorScriptView::scene_tab_changed()
{
    _update_components();
}

bool OrchestratorScriptView::is_modified() const
{
    return _orchestration->is_edited();
}

void OrchestratorScriptView::reload_from_disk()
{
    Ref<OScript> script = _resource;
    if (script.is_valid())
        script->reload();
    else
        WARN_PRINT("Cannot reload resource type: " + _resource->get_class());
}

void OrchestratorScriptView::apply_changes()
{
    for (const Ref<OScriptNode>& node : _orchestration->get_nodes())
        node->pre_save();

    for (int i = 0; i < _tabs->get_tab_count(); i++)
        if (OrchestratorGraphEdit* graph = Object::cast_to<OrchestratorGraphEdit>(_tabs->get_child(i)))
            graph->apply_changes();

    if (ResourceSaver::get_singleton()->save(_resource, _resource->get_path()) != OK)
        OS::get_singleton()->alert(vformat("Failed to save %s", _resource->get_path()), "Error");

    _update_components();

    for (int i = 0; i < _tabs->get_tab_count(); i++)
        if (OrchestratorGraphEdit* graph = Object::cast_to<OrchestratorGraphEdit>(_tabs->get_child(i)))
            graph->post_apply_changes();

    for (const Ref<OScriptNode>& node : _orchestration->get_nodes())
        node->post_save();
}

void OrchestratorScriptView::rename(const String& p_new_file)
{
    _resource->set_path(p_new_file);
}

bool OrchestratorScriptView::save_as(const String& p_new_file)
{
    if (ResourceSaver::get_singleton()->save(_resource, p_new_file) == OK)
    {
        _resource->set_path(p_new_file);
        return true;
    }
    return false;
}

bool OrchestratorScriptView::build()
{
    BuildLog log;
    _orchestration->validate_and_build(log);

    _build_errors->clear();
    _build_errors->append_text(vformat("[b]File:[/b] %s\n\n", _resource->get_path()));
    if (log.has_errors() || log.has_warnings())
    {
        _build_errors_dialog->set_title("Orchestration Build Errors");
        for (const String& E : log.get_messages())
            _build_errors->append_text(vformat("* %s\n", E));

        _build_errors_dialog->popup_centered_ratio(0.5);
        return false;
    }
    else
    {
        _build_errors_dialog->set_title("Orchestration Validation Results");
        _build_errors->append_text(vformat("* [color=green]OK[/color]: Script is valid."));

        _build_errors_dialog->popup_centered_ratio(0.25);
        return true;
    }
}

void OrchestratorScriptView::_update_components()
{
    _graphs->update();
    _functions->update();
    _macros->update();
    _variables->update();
    _signals->update();
}

int OrchestratorScriptView::_get_tab_index_by_name(const String& p_name) const
{
    for (int i = 0; i < _tabs->get_tab_count(); i++)
    {
        if (OrchestratorGraphEdit* graph = Object::cast_to<OrchestratorGraphEdit>(_tabs->get_child(i)))
        {
            if (p_name.match(graph->get_name()))
                return i;
        }
    }
    return -1;
}

OrchestratorGraphEdit* OrchestratorScriptView::_get_or_create_tab(const StringName& p_tab_name, bool p_focus, bool p_create)
{
    // Lookup graph tab
    int tab_index = _get_tab_index_by_name(p_tab_name);
    if (tab_index >= 0)
    {
        // Tab found
        if (p_focus)
            _tabs->get_tab_bar()->set_current_tab(tab_index);

        return Object::cast_to<OrchestratorGraphEdit>(_tabs->get_tab_control(tab_index));
    }

    if (!p_create)
        return nullptr;

    // Create the graph and add it as a tab
    const Ref<OScriptGraph> script_graph = _orchestration->get_graph(p_tab_name);
    if (!script_graph.is_valid())
        return nullptr;

    OrchestratorGraphEdit* graph = memnew(OrchestratorGraphEdit(_plugin, script_graph));
    _tabs->add_child(graph);

    String tab_icon = "ClassList";
    if (graph->is_function())
        tab_icon = "MemberMethod";

    _tabs->set_tab_icon(_get_tab_index_by_name(p_tab_name), SceneUtils::get_editor_icon(tab_icon));

    // Setup connections
    graph->connect("nodes_changed", callable_mp(this, &OrchestratorScriptView::_on_graph_nodes_changed));
    graph->connect("focus_requested", callable_mp(this, &OrchestratorScriptView::_on_graph_focus_requested));
    graph->connect("collapse_selected_to_function", callable_mp(this, &OrchestratorScriptView::_collapse_selected_to_function).bind(graph));
    graph->connect("expand_node", callable_mp(this, &OrchestratorScriptView::_expand_node).bind(graph));
    graph->connect("validation_requested", callable_mp(this, &OrchestratorScriptView::build));

    if (p_focus)
        _tabs->get_tab_bar()->set_current_tab(_tabs->get_tab_count() - 1);

    return graph;
}

void OrchestratorScriptView::_show_available_function_overrides()
{
    if (OrchestratorGraphEdit* graph = _get_or_create_tab("EventGraph", false, false))
    {
        graph->set_spawn_position_center_view();

        OrchestratorGraphActionFilter filter;
        filter.context_sensitive = true;
        filter.context.graph = graph;
        filter.flags = OrchestratorGraphActionFilter::Filter_OverridesOnly;

        OrchestratorGraphActionMenu* menu = graph->get_action_menu();
        menu->set_initial_position(Window::WINDOW_INITIAL_POSITION_CENTER_SCREEN_WITH_MOUSE_FOCUS);
        menu->apply_filter(filter);
    }
}

void OrchestratorScriptView::_close_tab(int p_tab_index)
{
    if (OrchestratorGraphEdit* graph = Object::cast_to<OrchestratorGraphEdit>(_tabs->get_tab_control(p_tab_index)))
    {
        // Don't allow closing the main event graph tab
        if (graph->get_name().match("EventGraph"))
            return;

        Node* parent = graph->get_parent();
        parent->remove_child(graph);
        memdelete(graph);
    }
}

void OrchestratorScriptView::_on_close_tab_requested(int p_tab_index)
{
    if (p_tab_index >= 0 && p_tab_index < _tabs->get_tab_count())
        _close_tab(p_tab_index);
}

void OrchestratorScriptView::_on_graph_nodes_changed()
{
    _update_components();
}

void OrchestratorScriptView::_on_graph_focus_requested(Object* p_object)
{
    Ref<OScriptFunction> function = Object::cast_to<OScriptFunction>(p_object);
    if (function.is_valid())
        if (OrchestratorGraphEdit* graph = _get_or_create_tab(function->get_function_name(), true, true))
            graph->focus_node(function->get_owning_node_id());
}

void OrchestratorScriptView::_on_show_graph(const String& p_graph_name)
{
    _get_or_create_tab(p_graph_name, true, true);
}

void OrchestratorScriptView::_on_close_graph(const String& p_graph_name)
{
    const int tab_index = _get_tab_index_by_name(p_graph_name);
    if (tab_index >= 0)
        _close_tab(tab_index);
}

void OrchestratorScriptView::_on_graph_renamed(const String& p_old_name, const String& p_new_name)
{
    if (OrchestratorGraphEdit* graph = _get_or_create_tab(p_old_name, false, false))
        graph->set_name(p_new_name);
}

void OrchestratorScriptView::_on_focus_node(const String& p_graph_name, int p_node_id)
{
    if (OrchestratorGraphEdit* graph = _get_or_create_tab(p_graph_name, true, true))
        graph->focus_node(p_node_id);
}

void OrchestratorScriptView::_on_override_function()
{
    _show_available_function_overrides();
}

void OrchestratorScriptView::_on_toggle_component_panel(bool p_visible)
{
    _scroll_container->set_visible(p_visible);
}

void OrchestratorScriptView::_on_scroll_to_item(TreeItem* p_item)
{
    if (p_item && _scroll_container)
    {
        Tree* tree = p_item->get_tree();

        const Rect2 item_rect = tree->get_item_area_rect(p_item);
        const Rect2 tree_rect = tree->get_global_rect();
        const Rect2 view_rect = _scroll_container->get_rect();

        const float offset = tree_rect.get_position().y + item_rect.get_position().y;
        if (offset > view_rect.get_size().y)
            _scroll_container->set_v_scroll(offset);
    }
}

void OrchestratorScriptView::_add_callback(Object* p_object, const String& p_function_name, const PackedStringArray& p_args)
{
    // Get the script attached to the object
    Ref<Script> edited_script = p_object->get_script();
    if (!edited_script.is_valid())
        return;

    // Make sure that we're only applying the callback to the right resource
    if (edited_script.ptr() != _resource.ptr())
        return;

    // Check if the method already exists and return if it does.
    if (_orchestration->has_function(p_function_name))
        return;

    MethodInfo mi;
    mi.name = p_function_name;
    mi.return_val.type = Variant::NIL;

    for (const String& argument : p_args)
    {
        PackedStringArray bits = argument.split(":");
        const BuiltInType type = ExtensionDB::get_builtin_type(bits[1]);

        PropertyInfo pi;
        pi.name = bits[0];
        pi.class_name = bits[1];
        pi.type = type.type;
        mi.arguments.push_back(pi);
    }

    OScriptNodeInitContext context;
    context.method = mi;

    OrchestratorGraphEdit* event_graph = _get_or_create_tab("EventGraph", true, false);
    if (!event_graph)
        return;

    const Ref<OScriptNodeEvent> node = event_graph->get_owning_graph()->create_node<OScriptNodeEvent>(context);
    if (node.is_valid())
    {
        _update_components();
        event_graph->focus_node(node->get_id());
    }
}

void OrchestratorScriptView::_notification(int p_what)
{
    if (p_what == NOTIFICATION_READY)
    {
        _main_view->connect("toggle_component_panel", callable_mp(this, &OrchestratorScriptView::_on_toggle_component_panel));

        Node* editor_node = get_tree()->get_root()->get_child(0);
        editor_node->connect("script_add_function_request", callable_mp(this, &OrchestratorScriptView::_add_callback));

        VBoxContainer* panel = memnew(VBoxContainer);
        panel->set_h_size_flags(SIZE_EXPAND_FILL);
        add_child(panel);

        MarginContainer* margin = memnew(MarginContainer);
        margin->set_v_size_flags(SIZE_EXPAND_FILL);
        panel->add_child(margin);

        _tabs = memnew(TabContainer);
        _tabs->get_tab_bar()->set_tab_close_display_policy(TabBar::CLOSE_BUTTON_SHOW_ACTIVE_ONLY);
        _tabs->get_tab_bar()->connect("tab_close_pressed", callable_mp(this, &OrchestratorScriptView::_on_close_tab_requested));
        margin->add_child(_tabs);

        _scroll_container = memnew(ScrollContainer);
        _scroll_container->set_horizontal_scroll_mode(ScrollContainer::SCROLL_MODE_DISABLED);
        _scroll_container->set_vertical_scroll_mode(ScrollContainer::SCROLL_MODE_AUTO);
        add_child(_scroll_container);

        VBoxContainer* vbox = memnew(VBoxContainer);
        vbox->set_h_size_flags(SIZE_EXPAND_FILL);
        _scroll_container->add_child(vbox);

        _build_errors = memnew(RichTextLabel);
        _build_errors->set_use_bbcode(true);
        _build_errors->connect("meta_clicked", callable_mp(this, &OrchestratorScriptView::_meta_clicked));

        _build_errors_dialog = memnew(AcceptDialog);
        _build_errors_dialog->set_title("Orchestrator Build Errors");
        _build_errors_dialog->add_child(_build_errors);
        add_child(_build_errors_dialog);

        _graphs = memnew(OrchestratorScriptGraphsComponentPanel(_orchestration));
        _graphs->connect("show_graph_requested", callable_mp(this, &OrchestratorScriptView::_on_show_graph));
        _graphs->connect("close_graph_requested", callable_mp(this, &OrchestratorScriptView::_on_close_graph));
        _graphs->connect("focus_node_requested", callable_mp(this, &OrchestratorScriptView::_on_focus_node));
        _graphs->connect("graph_renamed", callable_mp(this, &OrchestratorScriptView::_on_graph_renamed));
        _graphs->connect("scroll_to_item", callable_mp(this, &OrchestratorScriptView::_on_scroll_to_item));
        vbox->add_child(_graphs);

        _functions = memnew(OrchestratorScriptFunctionsComponentPanel(_orchestration, this));
        _functions->connect("show_graph_requested", callable_mp(this, &OrchestratorScriptView::_on_show_graph));
        _functions->connect("close_graph_requested", callable_mp(this, &OrchestratorScriptView::_on_close_graph));
        _functions->connect("focus_node_requested", callable_mp(this, &OrchestratorScriptView::_on_focus_node));
        _functions->connect("override_function_requested", callable_mp(this, &OrchestratorScriptView::_on_override_function));
        _functions->connect("graph_renamed", callable_mp(this, &OrchestratorScriptView::_on_graph_renamed));
        _functions->connect("scroll_to_item", callable_mp(this, &OrchestratorScriptView::_on_scroll_to_item));
        vbox->add_child(_functions);

        _macros = memnew(OrchestratorScriptMacrosComponentPanel(_orchestration));
        _macros->connect("scroll_to_item", callable_mp(this, &OrchestratorScriptView::_on_scroll_to_item));
        vbox->add_child(_macros);

        _variables = memnew(OrchestratorScriptVariablesComponentPanel(_orchestration));
        _variables->connect("scroll_to_item", callable_mp(this, &OrchestratorScriptView::_on_scroll_to_item));
        vbox->add_child(_variables);

        _signals = memnew(OrchestratorScriptSignalsComponentPanel(_orchestration));
        _signals->connect("scroll_to_item", callable_mp(this, &OrchestratorScriptView::_on_scroll_to_item));
        vbox->add_child(_signals);

        // The base event graph tab
        _event_graph = _get_or_create_tab("EventGraph");

        _update_components();
    }
}

OrchestratorScriptView::OrchestratorScriptView(OrchestratorPlugin* p_plugin, OrchestratorMainView* p_main_view, const Ref<OScript>& p_script)
    : _resource(p_script)
{
    _plugin = p_plugin;
    _main_view = p_main_view;
    _orchestration = p_script->get_orchestration();

    set_v_size_flags(SIZE_EXPAND_FILL);
    set_h_size_flags(SIZE_EXPAND_FILL);
}