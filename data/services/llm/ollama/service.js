function escapeHtml(value) {
    return String(value)
        .replace(/&/g, "&amp;")
        .replace(/</g, "&lt;")
        .replace(/>/g, "&gt;")
        .replace(/\n/g, "<br>");
}

function promptFor(capability, params, prompts) {
    var source = params.source_language_name || params.from_name || params.from || "auto";
    var target = params.to_name || params.to || "ru";
    var text = params.text || params.word || "";

    if (capability === "translate") {
        return (prompts.translate || "Translate from {source} to {target}:\n{text}")
            .replace("{source}", source)
            .replace("{target}", target)
            .replace("{text}", text) + (params.context ? "\n\nScene context (do not translate; keep names and terminology consistent):\n" + String(params.context).trim() : "") + (params.instruction ? "\n\n" + String(params.instruction).trim() : "");
    }
    if (capability === "summarize") {
        return (prompts.summarize || "Summarize:\n{text}").replace("{text}", text);
    }
    if (capability === "paraphrase") {
        return (prompts.paraphrase || "Rewrite:\n{text}").replace("{text}", text);
    }
    return (prompts.dictionary || "Explain \"{text}\" in {target}.")
        .replace("{text}", text)
        .replace("{target}", target);
}

function systemPromptFor(params) {
    var source = params.source_language_name || params.from_name || params.from || "auto";
    var target = params.to_name || params.to || "ru";
    var text = params.text || params.word || "";
    var prompt = String(settings.system_prompt || "").trim();
    if (!prompt) {
        prompt = "You are a precise assistant. Follow the user instruction exactly.";
    }
    return prompt
        .replace(/\{src\}/g, source)
        .replace(/\{dst\}/g, target)
        .replace(/\{text\}/g, text);
}

function normalizeEndpoint(value) {
    var endpoint = String(value || "http://127.0.0.1:11434/v1").trim().replace(/\/+$/, "");
    if (/\/chat\/completions$/.test(endpoint)) {
        return endpoint;
    }
    if (/\/v1$/.test(endpoint)) {
        return endpoint + "/chat/completions";
    }
    return endpoint + "/v1/chat/completions";
}

function execute(capability, params) {
    var prompts = loadJSON("prompts.json");
    var endpoint = normalizeEndpoint(settings.endpoint);
    var model = String(settings.model || "").trim();
    var temperature = Number(settings.temperature);
    if (!model) {
        return { error: "Ollama: выберите модель в настройках сервиса." };
    }
    if (isNaN(temperature)) {
        temperature = 0.2;
    }

    var headers = { "Content-Type": "application/json" };
    if (settings.api_key) {
        headers.Authorization = "Bearer " + settings.api_key;
    }

    var prompt = promptFor(capability, params, prompts);
    var body = {
        model: model,
        temperature: temperature,
        messages: [
            { role: "system", content: systemPromptFor(params) },
            { role: "user", content: prompt }
        ]
    };

    var response = http.post(endpoint, body, headers);
    if (response.error) {
        return { error: "Ollama: " + response.error };
    }

    var data;
    try {
        data = json.parse(response.body);
    } catch (error) {
        return { error: "Ollama: некорректный ответ сервера." };
    }

    if (response.status !== 200 || !data.choices || !data.choices.length) {
        var message = data && data.error && data.error.message;
        return { error: message || ("Ollama: HTTP " + response.status) };
    }

    var text = data.choices[0].message && data.choices[0].message.content
        ? data.choices[0].message.content : "";
    if (!text) {
        return { error: "Ollama: пустой ответ модели." };
    }
    if (capability === "dictionary") {
        return { html: "<p>" + escapeHtml(text) + "</p>", text: text };
    }
    return { text: text };
}
