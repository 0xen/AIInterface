"""M30 -- a node graph drawn with `aii_ui.NodeGraph`, in one window.

Copy this one level up (into %APPDATA%\\AIInterface\\scripts) to have it run,
or pass it with --script. It is a *policy*: started once at launch, on its
own thread, and it runs until the window is closed or the app quits.

Three fixed nodes -- Microphone, Recogniser, Claude -- wired Microphone ->
Recogniser -> Claude, each with a pin or two, one of them (Recogniser) with a
slider in its body. A minimap is on, so this also demonstrates the node
editor's own pan/zoom overview. `on_link` logs every link the user makes
through `aii.log`; a `text()` line above the editor shows the live link count
and node selection, read straight off the `NodeGraph` after each `draw()`.
"""

import aii
from aii_ui import NodeGraph, Panel


def _on_link(start, end):
    aii.log("ui_node_graph_demo: linked attr %d -> %d" % (start, end))
    return True


graph = NodeGraph(minimap=True)
graph.on_link = _on_link

mic = graph.add_node("Microphone", outputs=["audio"], pos=(20, 40))


def _recogniser_body(ui, node):
    # A widget inside a node needs an explicit width: ImGui's default is a
    # share of the *window*, which would stretch the node across the editor.
    # The value lives on the node dict so the user's drag survives the next
    # recording instead of snapping back to a literal.
    ui.set_next_item_width(120)
    node["confidence"] = ui.slider_float("confidence", node.get("confidence", 0.8), 0.0, 1.0)


recogniser = graph.add_node(
    "Recogniser", inputs=["audio"], outputs=["text"],
    pos=(240, 40), body=_recogniser_body)

claude = graph.add_node("Claude", inputs=["text"], pos=(460, 40))

graph.add_link(graph.pin(mic, "audio"), graph.pin(recogniser, "audio"))
graph.add_link(graph.pin(recogniser, "text"), graph.pin(claude, "text"))


def draw(ui):
    ui.text("links: %d   selected: %s" % (len(graph.links()), graph.selected_nodes()))
    ui.separator()
    graph.draw(ui)


Panel("ui_node_graph_demo", "Node Graph Demo", w=600, h=420, hz=10).run(draw)
