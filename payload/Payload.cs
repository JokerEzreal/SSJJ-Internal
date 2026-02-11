using UnityEngine;
using System.Runtime.CompilerServices;

namespace Payload
{
    /// <summary>
    /// Bridge between C# and C++.
    /// Each extern method maps to a C++ function registered via mono_add_internal_call.
    /// Callbacks are split so that an exception in one subsystem does not
    /// abort other subsystems within the same Unity event.
    /// </summary>
    public static class Bridge
    {
        // --- OnGUI (drawing) ---
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void OnDrawESP();

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void OnDrawMenu();

        // --- Update (per-frame logic) ---
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void OnUpdateCallback();

        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void OnAntiCheatCallback();

        // --- LateUpdate (post-frame) ---
        [MethodImpl(MethodImplOptions.InternalCall)]
        public static extern void OnLateUpdateCallback();
    }

    /// <summary>
    /// Entry point called from C++ after the assembly is loaded.
    /// Creates a persistent GameObject with the overlay MonoBehaviour.
    /// </summary>
    public static class Loader
    {
        public static void Init()
        {
            GameObject go = new GameObject("__SSJJ_Overlay__");
            // HideAndDontSave (61) = HideInHierarchy | DontSaveInEditor |
            // NotEditable | DontSaveInBuild | DontUnloadUnusedAsset.
            // This prevents game code from finding/destroying us via
            // GameObject.Find or FindObjectOfType, and also prevents
            // Unity from unloading us during scene transitions.
            go.hideFlags = HideFlags.HideAndDontSave;
            Object.DontDestroyOnLoad(go);
            var comp = go.AddComponent<OverlayBehaviour>();
            comp.hideFlags = HideFlags.HideAndDontSave;
        }
    }

    /// <summary>
    /// Persistent MonoBehaviour that relays Unity callbacks to C++.
    /// Each subsystem gets its own try-catch so that a failure in one
    /// (e.g. ESP hitting a destroyed entity during round transition)
    /// does not prevent other subsystems from running.
    /// </summary>
    public class OverlayBehaviour : MonoBehaviour
    {
        void OnGUI()
        {
            // ESP draws first (world-space overlays behind menu).
            // If ESP throws during a scene transition, menu still draws.
            try { Bridge.OnDrawESP(); }
            catch (System.Exception) { }

            try { Bridge.OnDrawMenu(); }
            catch (System.Exception) { }
        }

        void Update()
        {
            try { Bridge.OnUpdateCallback(); }
            catch (System.Exception) { }

            try { Bridge.OnAntiCheatCallback(); }
            catch (System.Exception) { }
        }

        void LateUpdate()
        {
            try { Bridge.OnLateUpdateCallback(); }
            catch (System.Exception) { }
        }
    }
}
