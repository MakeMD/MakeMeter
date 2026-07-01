#pragma once
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <thread>

// juce::String(const char*) is ASCII-only; non-ASCII literals need explicit UTF-8.
#define U8(s) juce::String (juce::CharPointer_UTF8 (s))

// Sends a measurement snapshot + question to a local Ollama server and returns mix advice.
//
// ponytail: defaults to local Ollama (free, private — you already run the stack). To use
//           Claude API instead, point endpoint at api.anthropic.com, swap the request/parse
//           JSON, and add the x-api-key header. Same ask()/callback shape.
class MixAdvisor
{
public:
    juce::String endpoint = "http://localhost:11434/api/generate";
    juce::String model    = "qwen3:14b";
    juce::String replyLang;   // "" (=Ukrainian) / a language name / "Auto (match input)"

    // TextEditor doesn't render Markdown — flatten **bold**, `code`, # headers, * bullets.
    static juce::String stripMarkdown (juce::String t)
    {
        const int te = t.lastIndexOf ("</think>");      // drop qwen3 reasoning block
        if (te >= 0) t = t.substring (te + 8);
        t = t.trim().replace ("**", "").replace ("__", "").replace ("`", "");
        auto lines = juce::StringArray::fromLines (t);
        for (auto& l : lines)
        {
            auto s = l.trimStart();
            while (s.startsWithChar ('#')) s = s.substring (1).trimStart();
            const bool bullet = s.startsWithChar ('*') || s.startsWithChar ('-');
            if (bullet) s = s.substring (1).trimStart();
            s = s.replace ("*", "");
            l = bullet ? "\xe2\x80\xa2 " + s : s; // • for bullets
        }
        return lines.joinIntoString ("\n");
    }

    // question + snapshot -> onResult(reply, ok). Callback fires on the message thread.
    void ask (const juce::String& snapshot,
              const juce::String& question,
              std::function<void (juce::String, bool)> onResult)
    {
        if (busy.exchange (true)) { onResult (U8 ("Зачекай — попередній запит ще обробляється."), false); return; }

        juce::String url = endpoint, mdl = model;
        juce::String langLine = "Reply in Ukrainian";                 // default when nothing is chosen
        if      (replyLang.startsWithIgnoreCase ("Auto")) langLine = "Reply in the same language as the QUESTION";
        else if (replyLang.isNotEmpty())                  langLine = "Reply in " + replyLang;

        // ponytail: prompt models the EchoJay assistant behaviour (read meters, never name the
        //           source/channel/genre, plain text, per-genre/channel ranges) — adapted, not copied.
        juce::String prompt =
            "You read audio meters and talk to an engineer about what the numbers show. You cannot "
            "hear the audio. Conversational and direct, like a mate in the studio. " + langLine +
            ". PLAIN TEXT only: no markdown, no asterisks, no #, no backticks, no em-dashes (use commas).\n\n"
            "INTERNAL CONTEXT - NEVER NAMED IN YOUR REPLY: the data gives a Channel type and Genre. Use "
            "them ONLY to judge what is normal (loudness range, width, crest). NEVER say the channel or "
            "genre out loud and never use a synonym for them. When you refer to the audio, only say: the "
            "signal, the track, this, it, what you've captured. Do not name instruments, sources or "
            "genres when analysing the capture (kick, vocal, bass, drums, hip-hop, EDM, etc.). Describe "
            "what the NUMBERS show, not what the source IS.\n\n"
            "ONLY use numbers present in the data. If a value isn't there, you don't have it - don't "
            "mention or invent it.\n\n"
            "CHANNEL SANITY CHECK FIRST: if the numbers clearly don't fit the (internal) channel type "
            "(e.g. a single element reading wide >50% and full-range, or a full mix below -20 LUFS), "
            "raise it as a measurement observation without naming the channel, then ask what it is.\n\n"
            "PER-CHANNEL NORMS (internal calibration only, never name the channel): full mix/master = "
            "wide-ish (width 30-55%), loud, crest 6-12; single elements = narrower/often mono, usually "
            "quieter; drum bus crest 12-25 (below 8 is over-compressed); vocal = mid-forward, crest 8-14, "
            "around -18..-12 LUFS; a bus sits between.\n\n"
            "RANGES (only flag if clearly outside): LUFS by genre - urban/pop -8..-11, dance/club -5..-8, "
            "rock -8..-12, jazz/classical/ambient -14..-22. Crest: <3 crushed, 3-5 compressed (fine for "
            "loud genres), 5-8 solid, 8-14 dynamic - do not assume compressed=bad. True peak: only flag "
            "above +2.0 dBTP; negative values (-0.2..-1.0) are healthy, never call them tight. Width/"
            "correlation: mention only if notably narrow/wide or a phase issue. Banded correlation: "
            "Corr-sub should sit near +1 (mono low end); flag below ~+0.7, a real problem below ~+0.3; "
            "Corr-mid 0.3-0.85 is normal width, not a problem; Corr-top can sit low on airy material. A "
            "band reading exactly 1.00 may just mean there is no content in that band, not perfect mono. "
            "Cite only the one band that tells the story. DC offset above ~2-3 mV is worth flagging. Do "
            "NOT manufacture problems: if numbers are in range, say so plainly.\n\n"
            "PLUGINS: recommend only tools in the engineer's list. Vary across the session, prefer their "
            "more interesting tools over the obvious big names, stock DAW plugins count. Sketch a 2-3 "
            "plugin chain only when asked broadly; for a single move give one suggestion. Give one or two "
            "concrete settings (Hz, dB, ms, ratio) when asked, not every parameter.\n\n"
            "If the message is a GENERAL question (technique, gear, references) rather than about this "
            "capture, just answer it like an engineer - there you MAY name instruments and genres "
            "normally; the no-naming rule only applies when commenting on the capture. Match technical "
            "depth to the engineer's experience and reference their monitors/headphones when relevant. "
            "Vary your opening and length; don't start the same way twice.\n\n"
            "=== METER DATA (channel/genre here are INTERNAL - do not name them) ===\n" + snapshot + "\n"
            "=== QUESTION ===\n" + (question.isEmpty() ? U8 ("Дай загальну оцінку — поведи від найхарактернішого в цифрах.") : question);

        std::thread ([this, url, mdl, prompt, onResult]
        {
            juce::var body (new juce::DynamicObject());
            body.getDynamicObject()->setProperty ("model", mdl);
            body.getDynamicObject()->setProperty ("prompt", prompt);
            body.getDynamicObject()->setProperty ("stream", false);
            const juce::String payload = juce::JSON::toString (body);

            juce::String reply; bool ok = false;
            juce::URL u (url);
            u = u.withPOSTData (payload);
            auto opts = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inPostData)
                            .withExtraHeaders ("Content-Type: application/json")
                            .withConnectionTimeoutMs (120000);

            if (auto stream = u.createInputStream (opts))
            {
                const juce::String resp = stream->readEntireStreamAsString();
                const juce::var parsed = juce::JSON::parse (resp);
                if (auto* o = parsed.getDynamicObject())
                {
                    reply = stripMarkdown (o->getProperty ("response").toString().trim());
                    ok = reply.isNotEmpty();
                }
                if (! ok) reply = U8 ("Не вдалося розпарсити відповідь Ollama:\n") + resp;
            }
            else
            {
                reply = U8 ("Немає зв'язку з ") + url + U8 (". Запущений Ollama? (`ollama serve`, модель `") + mdl + "`).";
            }

            busy.store (false);
            juce::MessageManager::callAsync ([onResult, reply, ok] { onResult (reply, ok); });
        }).detach();
    }

private:
    std::atomic<bool> busy { false };
};
