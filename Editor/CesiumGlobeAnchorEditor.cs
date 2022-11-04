using UnityEditor;
using UnityEngine;

namespace CesiumForUnity
{
    [CustomEditor(typeof(CesiumGlobeAnchor))]
    public class CesiumGlobeAnchorEditor : Editor
    {
        private CesiumGlobeAnchor _globeAnchor;

        private SerializedProperty _adjustOrientationForGlobeWhenMoving;
        private SerializedProperty _detectTransformChanges;
        private SerializedProperty _positionAuthority;

        // Converts the SerializedProperty's value to CesiumGeoreferenceOriginAuthority
        // enum it corresponds to, for convenience.
        internal CesiumGlobeAnchorPositionAuthority positionAuthority
        {   
            get
            {
                return (CesiumGlobeAnchorPositionAuthority)
                    this._positionAuthority.enumValueIndex;
            }
            set
            {
                // Since changing the position authority has more consequences, this setter
                // bypasses the SerializedProperty and invokes the globe anchor's setter
                // itself, in order to trigger its internal UpdateGlobePosition() method.
                if (value != this.positionAuthority)
                {
                    this._globeAnchor.positionAuthority = value;
                }
            }
        }

        private SerializedProperty _latitude;
        private SerializedProperty _longitude;
        private SerializedProperty _height;

        private SerializedProperty _ecefX;
        private SerializedProperty _ecefY;
        private SerializedProperty _ecefZ;

        private SerializedProperty _unityX;
        private SerializedProperty _unityY;
        private SerializedProperty _unityZ;

        private void OnEnable()
        {
            this._globeAnchor = (CesiumGlobeAnchor)this.target;

            this._adjustOrientationForGlobeWhenMoving =
                this.serializedObject.FindProperty("_adjustOrientationForGlobeWhenMoving");
            this._detectTransformChanges =
                this.serializedObject.FindProperty("_detectTransformChanges");
            this._positionAuthority =
                this.serializedObject.FindProperty("_positionAuthority");
            this._latitude = this.serializedObject.FindProperty("_latitude");
            this._longitude = this.serializedObject.FindProperty("_longitude");
            this._height = this.serializedObject.FindProperty("_height");
            this._ecefX = this.serializedObject.FindProperty("_ecefX");
            this._ecefY = this.serializedObject.FindProperty("_ecefY");
            this._ecefZ = this.serializedObject.FindProperty("_ecefZ");
            this._unityX = this.serializedObject.FindProperty("_unityX");
            this._unityY = this.serializedObject.FindProperty("_unityY");
            this._unityZ = this.serializedObject.FindProperty("_unityZ");
        }

        public override void OnInspectorGUI()
        {
            this.serializedObject.Update();

            DrawGlobeAnchorProperties();
            DrawLatitudeLongitudeHeightProperties();
            DrawEarthCenteredEarthFixedProperties();
            DrawUnityPositionProperties();

            this.serializedObject.ApplyModifiedProperties();

            base.OnInspectorGUI();
        }

        private void DrawGlobeAnchorProperties()
        {
            // The labels for this component are particularly long, so use a custom value
            // instead of the editor style's default.
            int labelWidth = 265;
            GUILayout.BeginHorizontal();
            GUIContent adjustOrientationContent = new GUIContent(
                "Adjust Orientation For Globe When Moving",
                "Whether to adjust the game object's orientation based on globe curvature " +
                "as the game object moves." +
                "\n\n" +
                "The Earth is not flat, so as we move across its surface, the direction of " +
                "\"up\" changes. If we ignore this fact and leave an object's orientation " +
                "unchanged as it moves over the globe surface, the object will become " +
                "increasingly tilted and eventually be completely upside-down when we arrive " +
                "at the opposite side of the globe." +
                "\n\n" +
                "When this setting is enabled, this component will automatically apply a " +
                "rotation to the Transform to account for globe curvature any time the game " +
                "object's position on the globe changes." +
                "\n\n" +
                "This property should usually be enabled, but it may be useful to disable it " +
                "when your application already accounts for globe curvature itself when it " +
                "updates a game object's transform, because in that case game object would " +
                "be over-rotated.");
            GUILayout.Label(adjustOrientationContent, GUILayout.Width(labelWidth));
            EditorGUILayout.PropertyField(
                this._adjustOrientationForGlobeWhenMoving,
                GUIContent.none);
            GUILayout.EndHorizontal();

            EditorGUI.BeginChangeCheck();
            GUILayout.BeginHorizontal();
            GUIContent detectTransformChangesContent = new GUIContent(
                "Detect Transform Changes",
                "Whether this component should detect changes to the Transform component, " +
                "such as from physics, and update the precise coordinates accordingly. " +
                "Disabling this option improves performance for game objects that will not " +
                "move. Transform changes are always detected in Edit mode, no matter the " +
                "state of this flag.");
            GUILayout.Label(detectTransformChangesContent, GUILayout.Width(labelWidth));
            EditorGUILayout.PropertyField(this._detectTransformChanges, GUIContent.none);
            GUILayout.EndHorizontal();
            if (EditorGUI.EndChangeCheck())
            {
                this._globeAnchor.StartOrStopDetectingTransformChanges();
            }

            GUIContent positionAuthorityContent = new GUIContent(
                "Position Authority",
                "The set of coordinates that authoritatively define the position of this game object."
            );
            this.positionAuthority = (CesiumGlobeAnchorPositionAuthority)
                EditorGUILayout.EnumPopup(positionAuthorityContent, this.positionAuthority);
        }

        private void DrawLatitudeLongitudeHeightProperties()
        {
            GUILayout.Label("Position (Longitude Latitude Height)", EditorStyles.boldLabel);

            CesiumGlobeAnchorPositionAuthority authority =
                (CesiumGlobeAnchorPositionAuthority)this._positionAuthority.enumValueIndex;

            EditorGUI.BeginDisabledGroup(
                authority != CesiumGlobeAnchorPositionAuthority.LongitudeLatitudeHeight);

            EditorGUI.EndDisabledGroup();
        }

        private void DrawEarthCenteredEarthFixedProperties()
        {

        }

        private void DrawUnityPositionProperties()
        {

        }
    }
}