"""用供应商格式的响应检查适配器，不把模拟响应当作真实模型延迟或效果"""
import io
import json
import os
import unittest
from unittest.mock import patch
from test_server import context
from server import decide, validate_action, validate_context


class JevTests(unittest.TestCase):
    def request(self):
        request = context()
        e = request["evidence"][0]
        request["allowed_actions"].extend(["StrikeOnce", "HoldVehicle"])
        request["candidates"] = [
            {"action": "Wait", "target_id": "", "duration": 1, "evidence_ids": []},
            {"action": "StrikeOnce", "target_id": e["subject_id"], "duration": 1.2,
             "evidence_ids": [e["event_id"]]}]
        return request

    def test_jev_choice_maps_exact_candidate(self):
        request = self.request()
        response = {"answers": {"action": {"type": "choice", "choice": "option_1", "confidence": .82}}}
        def transport(req, timeout):
            body = json.loads(req.data)
            self.assertEqual(req.full_url, "https://api.typesafe.ai/v1/systemone")
            self.assertEqual(timeout, 4)
            self.assertEqual(set(body["questions"]["action"]["criteria"]), {"option_0", "option_1"})
            return io.BytesIO(json.dumps(response).encode())
        with patch.dict(os.environ, {"TYPESAFE_API_KEY": "test-only"}), patch("jev.urllib.request.urlopen", transport):
            result = decide(request, "jev")
        self.assertEqual(result["action"], "StrikeOnce")
        self.assertEqual(result["target_id"], request["evidence"][0]["subject_id"])
        self.assertEqual(result["request_id"], request["request_id"])

    def test_jev_unknown_choice_and_invalid_confidence_rejected(self):
        for fields in ({"choice": "invented", "confidence": 1}, {"choice": "option_1", "confidence": float("nan")},
                       {"choice": "option_1", "confidence": True}):
            raw = json.dumps({"answers": {"action": {"type": "choice", **fields}}}).encode()
            with self.subTest(fields=fields), patch.dict(os.environ, {"TYPESAFE_API_KEY": "test-only"}), \
                 patch("jev.urllib.request.urlopen", return_value=io.BytesIO(raw)), self.assertRaises(ValueError):
                decide(self.request(), "jev")

    def test_candidate_pair_and_dialogue_limits(self):
        request = self.request()
        good = request["candidates"][1]
        self.assertEqual(validate_context(request), request)
        for mutation in ({"action": "HoldVehicle"}, {"speech": "x"*121}, {"emotion": "ExecuteCode"}):
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_action(request, {**good, **mutation})

    def test_hybrid_routes_dialogue_without_serial_jev(self):
        request = self.request()
        request["evidence"][0].update(kind="Speech", source="Heard")
        response = {"choices": [{"message": {"content": json.dumps({**request["candidates"][0], "speech": "你先把话说清楚。", "emotion": "Angry"})}}]}
        with patch.dict(os.environ, {"DELIVERY_AI_MODEL_URL": "https://model.invalid/chat", "DELIVERY_AI_MODEL": "test"}), \
             patch("server.urllib.request.urlopen", return_value=io.BytesIO(json.dumps(response).encode())), \
             patch("server.choose_jev") as jev:
            result = decide(request, "hybrid")
        jev.assert_not_called()
        self.assertEqual(result["speech"], "你先把话说清楚。")


if __name__ == "__main__":
    unittest.main()
