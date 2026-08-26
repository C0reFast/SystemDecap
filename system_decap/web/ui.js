function element(tag, className = "", content = null) {
  const node = document.createElement(tag);
  if (className) node.className = className;
  if (content !== null && content !== undefined) node.textContent = String(content);
  return node;
}

function append(parent, ...children) {
  for (const child of children.flat()) {
    if (child !== null && child !== undefined) parent.append(child);
  }
  return parent;
}

function button(label, className = "") {
  const node = element("button", className, label);
  node.type = "button";
  return node;
}


function sectionHeader(index, kicker, title, description) {
  const header = element("header", "section-heading");
  const heading = element("div");
  append(
    heading,
    element("span", "overline", `${String(index).padStart(2, "0")} / ${kicker}`),
    element("h2", "", title)
  );
  append(header, heading, element("p", "", description));
  return header;
}

function emptyCard(title, message) {
  const card = element("article", "chart-card empty-chart");
  append(card, element("h3", "", title), element("p", "", message));
  return card;
}


export { element, append, button, sectionHeader, emptyCard };
